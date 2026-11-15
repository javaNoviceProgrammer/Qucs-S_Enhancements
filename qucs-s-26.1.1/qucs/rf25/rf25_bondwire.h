/*
 * rf25_bondwire.h - RF 2.5D: bond wires, made of what the solver has.
 *
 * A wire looped in the air between two pads is no planar current, nor a
 * via's. It is made one: its profile - a parabolic arc between the pads, its
 * peak \a height over their chord, or a ball bond's loop, straight up from
 * the first pad, flat over it and straight down to the second - cut into
 * pieces along the span, each a run at the height that keeps the profile's
 * mean ln h over its piece (a wire's L per length over a ground goes as
 * ln(2h/a)), on a metal layer of its own in a slab of air added over the
 * stack; and the steps between the pieces and up from each pad, vias.
 *
 * The staircase's legs add their rise where the profile's adds in quadrature
 * to its span, and its corners are square, so its runs' heights over the
 * chord are then scaled, one factor for them all, until its inductance over
 * the ground - a Neumann integral of the path with its image, the wire's
 * radius a - is the profile's own (wireLoopInductance()); unless the wire
 * asks not to be.
 *
 * Its cross-sections are a round wire's at radio frequencies, where the
 * current is on its surface (a geometric mean distance of a):
 *  - each run a strip e^(3/2) a wide, a sheet's geometric mean distance with
 *    its current spread across it, w e^(-3/2), a; its metal w/pi thick at
 *    sigma pi^2 a^2/w^2, so that its resistance per length is the round
 *    wire's both at direct current, 1/(sigma pi a^2), and with the skin
 *    thin, Rs/(2 pi a);
 *  - each leg a square a/0.44705 across (a solid square's geometric mean
 *    distance is 0.44705 of its side), its conductivity sigma pi^2 a^2/(4
 *    s^2) so that its resistance with the skin thin is the round wire's; at
 *    direct current its resistance is then 4/pi of the wire's (said).
 *
 * The air slab has no contrast with what is over it, so the stack's own
 * fields are as they were; the wires' layers are named after them, and are
 * the solve's alone (Model::solveStack(), Layer::standsFor): the stack panel,
 * its drawing and the exports have the board's layers, the wires said.
 */
#ifndef QUCS_RF25_BONDWIRE_H
#define QUCS_RF25_BONDWIRE_H

#include "rf25_layout.h"
#include "rf25_model.h"

#include <QList>
#include <QPointF>
#include <QStringList>

namespace qucs_s::rf25 {

struct BondWire {
    QString name;                   ///< "W1"
    QString fromMetal, toMetal;     ///< its pads' metals
    QPointF from, to;               ///< its pads, the layout's units (µm)
    double height = 150e-6;         ///< m, the arc's peak over its pads' chord; a ball bond's loop over its first pad
    double diameter = 25e-6;        ///< m
    double sigma = 0;               ///< S/m; nought, perfect
    int pieces = 3;                 ///< the runs along its span: one to four (more is refused: rf25_bondwire.cpp says why)
    bool ball = false;              ///< its profile a ball bond's loop, not an arc
    double knee = 0.15;             ///< a ball bond's: the share of the span flat at its height
    bool fit = true;                ///< its runs' heights fitted to its profile's inductance
};

/// The wire's profile from \a zA to \a zB (m over the ground) along its span
/// \a span (m): its corners (s, z), an arc's in many short chords.
QList<QPointF> bondWirePath(const BondWire& wire, double zA, double zB, double span);

/// The inductance, H, of a wire of radius \a radius along \a path - points
/// (s, z), m, in a vertical plane, z over a perfect ground - with the ground
/// its return (its image below, horizontal currents reversed) or, \a ground
/// false, alone: Neumann's double integral, each part cut no longer than the
/// radius or a two-hundredth of the path, the kernel 1/sqrt(r^2 + a^2).
double wireLoopInductance(const QList<QPointF>& path, double radius, bool ground = true);

/// \a stack and \a conductors with \a wires made of air slabs, metal layers
/// and vias, as above; why not where a wire cannot be.
struct BondWired {
    Stack stack;
    QList<Conductor> conductors;
    QStringList problems, lines;
    bool ok() const { return problems.isEmpty(); }
};
BondWired withBondWires(const Stack& stack, const QList<Conductor>& conductors, const QList<BondWire>& wires);

/// The model's bond wires (Layout > Shapes, rf_bondwire), each evaluated in
/// its parameters; one that cannot be read left out and said.
QList<BondWire> bondWiresOf(const Model& model, QStringList* problems = nullptr);

/// What a file of the board says of \a model's bond wires, \a where (\"an
/// XML stackup\"): each is no layer of the board but the solve's stand-ins
/// (Model::solveStack()), so not written - its pads, height and diameter for
/// it to be drawn as a wire there.
QStringList bondWiresNotWritten(const Model& model, const QString& where);

/// The heights (m) of a wire's pieces over the ground: each the exponential
/// of the arc's mean ln h over its piece, the arc z(u) = zA (1 - u) + zB u + 4
/// height u (1 - u); or of \a wire's profile, unfitted.
QList<double> bondWireLevels(double zA, double zB, double height, int pieces);
QList<double> bondWireLevels(const BondWire& wire, double zA, double zB);

} // namespace qucs_s::rf25

#endif
