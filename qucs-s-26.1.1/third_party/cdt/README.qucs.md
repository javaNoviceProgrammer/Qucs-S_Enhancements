# CDT in Qucs-S

[CDT](https://github.com/artem-ogre/CDT), constrained Delaunay triangulation
in C++, for the multiphysics solver's mesher (`qucs/multiphysics/fem_mesh.cpp`):
the triangulation of a geometry's boundaries, and Ruppert's refinement of its
triangles' smallest angles.

- **Version**: 2.0.0 (20 September 2026), the first with Ruppert's
  refinement: `CDT-2.0.0.tar.gz` from GitHub, SHA-256
  `81ed0c7a8cbdc059b26948a160c3ce68c7a147976334093fe3175c8fa161a447`.
- **Licence**: MPL 2.0 (`LICENSE`), which may be combined with the GPL. Its
  `predicates.h` (Shewchuk's robust predicates, William C. Lenthe's version)
  is under the BSD licence printed at its top.
- **What is here**: `CDT/include/` of that release and its `LICENSE`; not
  its tests, visualizer or build files. `CMakeLists.txt` is ours: an
  interface library of the headers, used header-only (no
  `CDT_USE_AS_COMPILED_LIBRARY`).
- **Changed here** (each marked "Qucs-S" in it): `Triangulation::unsplitEdges`,
  fixed edges `refineTriangles` never splits for being encroached - a
  layered region's sides, its stretched triangles left as they are; a
  triangle whose circumcenter would encroach one is given up on, as one by
  a too short edge is, counted in `Unrefined::unsplitEdges`
  (`Triangulation.h`; `resolveEncroachedEdges` and `Unrefined`'s
  constructor in `Triangulation.hpp`).

To take a newer version: copy its `CDT/include/` over `include/`, make the
change above again, and run the `test_fem_*` tests, under the sanitizers too.
