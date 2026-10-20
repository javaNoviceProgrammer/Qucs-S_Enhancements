/*
 * fem_process.h - a process flow: a 2D cross-section made by its steps in
 *                 order - a substrate or an epitaxial stack, then deposits,
 *                 etches, planarizing, strips, epitaxy and fills (masks from
 *                 a layout's layers along a cut, or typed in) - its regions
 *                 the geometry's objects, named by the steps that made them;
 *                 and its dopants: implants, diffusion, oxidation, the III-V
 *                 steps, carried on a grid through every step
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_PROCESS_H
#define QUCS_FEM_PROCESS_H

#include "fem_model.h"

#include <QPointF>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace qucs_s::fem {

/// A region of a process state: what a step made of one material, by its
/// name (the step's), its outlines counterclockwise, holes clockwise, in
/// the geometry's unit. Crystalline: grown (a substrate, a stack's layer,
/// epitaxy) - what epitaxy grows on, what a crystal-plane etch follows.
struct ProcessRegion {
    QString name;
    QString material;
    bool crystal = false;
    std::vector<std::vector<QPointF>> loops;
    double area() const;
};

/*!
 * A process flow's dopants, on a grid over its window: cells between the
 * edges xe (in x) and ye (in y), the geometry's unit; each cell's material
 * (an index into materials; -1 air). Each field's values by cell, x
 * fastest: a species' concentration (m⁻³), all of it and its active part;
 * the point defects' excess over their equilibrium ("I", "V", m⁻³); a III-V
 * sublattice's share of an element ("x:In", "x:Ga", "x:As"... - an
 * intermixed well's composition).
 */
struct ProcessDopants {
    std::vector<double> xe, ye;
    QStringList materials;
    /// By material: what it is as a host - 4 a group IV semiconductor
    /// (silicon, germanium, SiGe), 35 a III-V, 26 a II-VI, 0 anything else.
    std::vector<int> hosts;
    std::vector<int> material;
    struct Field {
        QString name;
        std::vector<double> total, active;
    };
    std::vector<Field> fields;
    int nx() const { return int(xe.size()) - 1; }
    int ny() const { return int(ye.size()) - 1; }
    std::size_t index(int i, int j) const { return std::size_t(j) * std::size_t(nx()) + std::size_t(i); }
    /// The cell holding (x, y), its column and row; false outside the grid.
    bool cellAt(double x, double y, int* i, int* j) const;
    const Field* field(const QString& name) const;
    Field* field(const QString& name);
    /// The fields of species (not "I", "V", nor compositions).
    std::vector<const Field*> species() const;
    /// A field's value at (x, y) as material \a m has it: bilinear among its
    /// cells there (the nearest of them, at an interface); NaN where there
    /// are none near.
    double at(const Field& f, double x, double y, int m, bool active = true) const;
    /// The active donors and acceptors at (x, y) in material \a m (m⁻³):
    /// each species by what it is in that host (boron an acceptor in
    /// silicon, zinc in InP...).
    void carriers(double x, double y, int m, double* donors, double* acceptors) const;
    /// The material holding (x, y) as the grid has it (-1: air).
    int materialAt(double x, double y) const;
};

/// What a species is in a host (ProcessDopants::hosts): +1 a donor, -1 an
/// acceptor, 0 neither (an isolation implant's H, He, Fe...).
int dopantCharge(const QString& species, int host);

/// A process flow's state after a step: its regions, its window (x0 to
/// x1, the substrate's bottom at y = bottom), its dopants (null before a
/// step that has any).
struct ProcessState {
    std::vector<ProcessRegion> regions;
    double x0 = 0, x1 = 0, bottom = 0;
    std::shared_ptr<const ProcessDopants> dopants;
    /// The highest point of its solids (bottom when it has none).
    double top() const;
};

/// A step run: its tag and name, what it did (a line), its time, warnings.
struct ProcessStepReport {
    QString tag, name, said;
    double ms = 0;
    QStringList warnings;
};

/// A flow run up to a step: the state then, each step's report; an error
/// (and the step it stopped at), else empty.
struct ProcessRun {
    ProcessState state;
    std::vector<ProcessStepReport> steps;
    QString error, failedAt;
    QStringList warnings;
    /// The layout a mask read (an absolute path), when one did.
    QStringList files;
    bool ok() const { return error.isEmpty(); }
};

/// Runs the process flow \a flow (a "process" node) of \a model, its
/// parameters evaluated in \a parameters, the geometry's unit \a unitScale
/// metres: up to the step \a until (inclusive; empty: all of them).
ProcessRun runProcess(const Node& flow, const Model& model, const ParameterScope& parameters, double unitScale,
                      const QString& until = {});

/// The objects' names a flow makes, in order: each step's that makes a
/// region (for the materials' and physics' selections).
QStringList processObjectNames(const Node& flow);
/// Whether a step of this type makes a region of its own (a substrate, a
/// deposit, epitaxy, a fill, an oxide); a stack's rows make theirs.
bool processStepMakesRegion(const QString& type);

} // namespace qucs_s::fem

#endif // QUCS_FEM_PROCESS_H
