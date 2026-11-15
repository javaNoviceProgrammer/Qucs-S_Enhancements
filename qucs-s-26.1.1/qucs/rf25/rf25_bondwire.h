/*
 * rf25_bondwire.h - RF 2.5D: bond wires, made of what the solver has.
 *
 * A wire arched in the air between two pads is no planar current, nor a
 * via's. It is made one: its profile - a parabolic arc between the pads, its
 * peak \a height over their chord - cut into pieces along the span, each a
 * run at the height that keeps the arc's mean ln h over its piece (a wire's
 * L per length over a ground goes as ln(2h/a), so that each piece's L is the
 * arc's there), on a metal layer of its own in a slab of air added over the
 * stack; and the steps between the pieces and up from each pad, vias.
 *
 *  - Each run is a strip 4a wide, a flat strip's external inductance a
 *    round wire's of radius a; a leg a square 0.871 d across, its geometric
 *    mean distance a round wire's.
 *  - The strips' metal is thick 4a/pi and conducts sigma (pi/4)^2, so that
 *    both at direct current and with the skin thin a strip's resistance per
 *    length is the round wire's, 1/(sigma pi a^2) and Rs/(2 pi a).
 *
 * The air slab has no contrast with what is over it, so the stack's own
 * fields are as they were; the wires' layers are named after them.
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
    double height = 150e-6;         ///< m, the arc's peak over its pads' chord
    double diameter = 25e-6;        ///< m
    double sigma = 0;               ///< S/m; nought, perfect
    int pieces = 5;                 ///< the runs along its span
};

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

/// The heights (m) of a wire's pieces over the ground: each the exponential
/// of the arc's mean ln h over its piece, the arc z(u) = zA (1 - u) + zB u + 4
/// height u (1 - u).
QList<double> bondWireLevels(double zA, double zB, double height, int pieces);

} // namespace qucs_s::rf25

#endif
