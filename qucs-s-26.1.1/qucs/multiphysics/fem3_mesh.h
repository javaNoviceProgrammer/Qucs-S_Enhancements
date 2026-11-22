/*
 * fem3_mesh.h - a 3D multiphysics model's mesh: prisms swept from a 2D mesh
 *               of its plan, each slab divided in z - graded from thin
 *               layers at the growth rate, or so many across a domain as
 *               asked - every node a plan node at a height
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_MESH_H
#define QUCS_FEM3_MESH_H

#include "fem3_geometry.h"
#include "fem_mesh.h"

#include <array>
#include <memory>
#include <vector>

namespace qucs_s::fem {

/*!
 * A mesh of prisms over a 3D geometry's domains: each a triangle of the plan's
 * mesh between two heights - its bottom's three nodes counter-clockwise seen
 * from above, then its top's above them. A node is a plan node at a height,
 * as nodeOrigin says (what a cut, an extrusion, the bridge to 2D use); nodes
 * no prism uses are left out. Its boundary faces lie on the geometry's faces:
 * a floor's triangles, a wall's quadrilaterals, each counter-clockwise seen
 * from its face's front (that face's normal's side).
 *
 * By band (the mesh's Plan "band"): the heights in bands, each band's prisms
 * of a plan of its own (Band) - coarse away from the fine objects' heights,
 * the footprints of objects absent from it merged. Two bands meet on a
 * height (an interface, in the air), each with its own nodes there; a
 * prism's plan is its band's (planOf()), its node origin its band's plan
 * node. One band: as before, its plan the mesh's.
 */
struct Mesh3 {
    std::vector<Vec3> nodes;
    std::vector<std::pair<int, int>> nodeOrigin;   ///< each node's plan node and height
    std::vector<double> heights;                   ///< every height, rising: the levels and their divisions
    std::vector<int> levelHeight;                  ///< each level's index in heights
    std::vector<std::array<int, 6>> prisms;
    std::vector<int> domain;                       ///< each prism's
    std::vector<int> prismTriangle;                ///< its plan triangle (its band's plan's)
    std::vector<int> prismLayer;                   ///< its bottom's height
    /// A band of heights and its plan: heights first..last (the last its
    /// top, the next band's first), its nodes by height and plan node.
    struct Band {
        std::shared_ptr<const Mesh> plan;
        int first = 0, last = 0;
        std::vector<int> nodeAt;                   ///< [(height - first) * plan nodes + plan node]: the node, or -1
        /// Its plan's merged plan domains: each one's representative.
        std::vector<int> merged;
    };
    std::vector<Band> bands;                       ///< one at least (by band: more)
    /// By interface (between band i and i + 1): the band whose degrees of
    /// freedom there are constrained, the finer (a node, an edge or a
    /// triangle the two bands share there is one, not constrained).
    std::vector<int> interfaceFiner;
    std::vector<int> prismBand;                    ///< each prism's
    struct Face {
        std::array<int, 4> nodes{{-1, -1, -1, -1}};
        bool quad = false;                         ///< a wall's: four nodes, else a floor's three
        int face = -1;                             ///< the geometry's face
        int plan = -1;                             ///< a floor's plan triangle; a wall's plan edge (Mesh::edges)
        int height = -1;                           ///< a floor's height; a wall's bottom's
        int band = 0;                              ///< whose plan
    };
    std::vector<Face> faces;
    std::vector<int> pointNode;                    ///< each point of the geometry's node
    std::shared_ptr<const Mesh> plan;
    std::shared_ptr<const Topology3> topology;
    double unitScale = 1e-3;

    // Its quality: the plan's (its smallest angle, its worst triangle's
    // quality), and the prisms' stretch - a prism's height over its
    // triangle's size, or the size over the height, the larger: 1 for one
    // as tall as it is wide - the worst and the mean; its volume.
    double minAngle = 0;
    double minQuality = 0;
    double maxStretch = 0;
    double meanStretch = 0;
    double volume = 0;

    /// [height * the plan's nodes + plan node]: the node, or -1 (one no prism
    /// uses).
    std::vector<int> nodeAt;
    /// Each node's band (where two meet, its own side's).
    std::vector<int> nodeBand;

    /// A node by plan node and height, or -1 (band 0's plan; by band, the
    /// band's that holds the height, the lower at an interface).
    int node(int planNode, int height) const;
    /// A node by band, its plan's node and height, or -1.
    int node(int band, int planNode, int height) const;
    /// Prism \a p's plan.
    const Mesh& planOf(int p) const { return *bands[std::size_t(prismBand[std::size_t(p)])].plan; }
    bool byBand() const { return bands.size() > 1; }
    /// Its plans' triangles (by band, all the bands').
    std::size_t planTriangles() const
    {
        if (!byBand()) return plan ? plan->triangles.size() : 0;
        std::size_t n = 0;
        for (const Band& b : bands) n += b.plan ? b.plan->triangles.size() : 0;
        return n;
    }
    void computeStatistics();
};

/// A 3D mesh built, or why not.
struct Mesh3Build {
    std::shared_ptr<Mesh3> mesh;
    QString error;
    QStringList warnings;
};

/// A slab's divisions by size at most (a Distribution's count is clamped
/// the same).
constexpr int MaxSweptDivisions = 100000;
/// A mesh's prisms at most, and its plan nodes times its heights: past
/// them it is refused before it is made, said, not run out of memory.
constexpr long long MaxPrisms = 20000000;
constexpr long long MaxNodeSlots = 200000000;

/// The divisions of slabs \a levels (rising) in z: each slab's heights
/// between its levels, its own excluded. \a hmax by slab (its largest
/// element, infinite: none), \a count by slab (0: by size), \a hLevel by
/// level (an element's height at it, at most; infinite: none), \a growth the
/// rate an element's height grows at from a thin slab or a level asked fine.
std::vector<std::vector<double>> sweptDivisions(const std::vector<double>& levels, const std::vector<double>& hmax,
                                                const std::vector<int>& count, const std::vector<double>& hLevel, double growth);

/// Meshes 3D \a model's geometry \a topology as its mesh node says: its plan
/// meshed to its sizes (the Size features' domains, faces and points taken to
/// the plan), each slab divided in z, the triangles swept through the slabs
/// their domains fill. One past MaxPrisms, or out of memory, is an error.
Mesh3Build buildMesh3(const Model& model, const ParameterScope& parameters, std::shared_ptr<const Topology3> topology,
                      const Progress& progress = {});

} // namespace qucs_s::fem

#endif // QUCS_FEM3_MESH_H
