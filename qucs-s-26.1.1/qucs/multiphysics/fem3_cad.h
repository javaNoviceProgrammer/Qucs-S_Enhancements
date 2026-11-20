/*
 * fem3_cad.h - a 3D multiphysics model's geometry and mesh to other tools'
 *              formats, and solids read from theirs: each domain a closed
 *              shell (STL, OBJ, a faceted STEP), the prism mesh with its
 *              domains and faces (Gmsh's MSH 4.1, Abaqus's INP, Nastran's
 *              bulk data), triangulated solids read (STL, OBJ) and cut into
 *              the slabs of a layered geometry - a prism's exactly, any other
 *              solid as a stack of slices
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_CAD_H
#define QUCS_FEM3_CAD_H

#include "fem3_geometry.h"
#include "fem3_mesh.h"

#include <QByteArray>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <array>
#include <vector>

namespace qucs_s::fem {

/// A closed shell of a domain: its triangles, each wound with its normal
/// out of the domain, over \a points; \a faces each triangle's face of the
/// geometry.
struct DomainShell {
    QString name;                              ///< the domain's ("d3_core": its number from 1 and its solid's)
    std::vector<Vec3> points;                  ///< (the geometry's unit)
    std::vector<std::array<int, 3>> triangles;
    std::vector<int> faces;
    double volume() const;                     ///< the triangles' signed volume
};
/// Each domain of \a topology (or those where \a only is set, by domain) as
/// a closed shell: the triangles of its faces, their points welded, wound
/// alike and out of it.
std::vector<DomainShell> domainShells(const Topology3& topology, const std::vector<char>& only = {});

/// The shells as an STL: ASCII, a solid each, or (\a binary) one solid of
/// them all, coordinates in millimetres (\a mm, else the geometry's unit).
QByteArray stlOf(const std::vector<DomainShell>& shells, double unitScale, bool binary, bool mm = true);
/// As a Wavefront OBJ: an object each, their points shared, in the
/// geometry's unit (a comment says which).
QByteArray objOf(const std::vector<DomainShell>& shells, const Topology3& topology);
/// As a STEP AP214 file (ISO 10303-21): a manifold solid B-rep each, named,
/// of planar faces - a planar face of the geometry one, bounded by its
/// outlines and holes; a curved wall's triangles each one - their edges
/// lines shared by the faces on them, in millimetres; \a name the
/// product's.
QByteArray stepOf(const std::vector<DomainShell>& shells, const Topology3& topology, const QString& name);

/// The prism mesh in Gmsh's MSH 4.1 (ASCII): its prisms each in its domain's
/// volume, the faces of the geometry its surfaces (triangles and quads), a
/// physical group each, named; coordinates in the geometry's unit.
QByteArray mshOf(const Mesh3& mesh);
/// In Abaqus's input format: its nodes, its prisms (C3D6) in an element
/// set by domain, its faces' triangles and quads (S3, S4) in one by face.
QByteArray inpOf(const Mesh3& mesh);
/// In Nastran's bulk data (large fields): GRID, its prisms CPENTA with a
/// PSOLID by domain, its faces CTRIA3 and CQUAD4 with a PSHELL by face.
QByteArray bdfOf(const Mesh3& mesh);
/// A prism's six nodes ordered as these formats want: its bottom triangle
/// wound about its rise (the volume positive).
std::array<int, 6> orientedPrism(const Mesh3& mesh, int prism);

/// Triangles read from a file: an STL (ASCII or binary) or a Wavefront OBJ
/// (its polygons fanned), by its contents - each in a group where the file
/// has them (an ASCII STL's solids, an OBJ's objects or groups), named.
struct TriangleSoup {
    std::vector<std::array<Vec3, 3>> triangles;
    std::vector<int> group;   ///< by triangle (empty: none)
    QStringList names;        ///< by group
    QString error;
};
TriangleSoup readTriangles(const QByteArray& data, const QString& fileName = {});

/// A solid read cut into slabs: each slab from \a z0 to \a z1 its section's
/// loops in the plan (outlines counter-clockwise, holes clockwise).
struct SlabbedSolid {
    struct Slab {
        double z0 = 0, z1 = 0;
        std::vector<std::vector<QPointF>> loops;
    };
    std::vector<Slab> slabs;
    bool prismatic = true;   ///< its faces level or upright: its slabs exact
    int group = -1;          ///< the file's group it is of (-1: none)
};
/// \a triangles split into solids - by \a groups (by triangle; empty:
/// none), then those that share edges -, each cut into slabs: a prismatic one
/// (each face level or upright) at its own levels, exactly - neighbouring
/// slabs alike made one -; any other into \a slices slabs of its height,
/// each its section at its middle. \a warnings says where it was not exact,
/// or a body was not closed.
std::vector<SlabbedSolid> slabSolids(const std::vector<std::array<Vec3, 3>>& triangles, int slices, QStringList* warnings,
                                     const std::vector<int>& groups = {});

} // namespace qucs_s::fem

#endif // QUCS_FEM3_CAD_H
