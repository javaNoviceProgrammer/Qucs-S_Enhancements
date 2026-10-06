/*
 * textplacement.h - where a schematic's texts are, what each is drawn over
 *                   (a wire, a symbol, another part's text, a net label, a
 *                   diagram, a painting), and a free spot beside a part for
 *                   its text
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_TEXTPLACEMENT_H
#define QUCS_TEXTPLACEMENT_H

#include <QLine>
#include <QPolygonF>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

#include <optional>

class Component;
class Diagram;
class Schematic;
class WireLabel;

namespace qucs_s::textplace {

/// A thing a text can be drawn over.
struct Thing {
    enum Kind { Wire, Symbol, Text, Label, Diagram, Painting };
    Kind kind = Wire;
    QRect box;                    ///< where it is (a wire's: the rectangle of its two ends)
    QRect ink;                    ///< a text's letters, tight (tested in place of its box)
    QLine line;                   ///< a wire's ends
    const void* owner = nullptr;  ///< the part a symbol or text is of, the label, the diagram, the painting, the wire
    QString name;                 ///< as it is said: "the wire 340,260-340,420", "R1's symbol", "C2's text"
    /// A symbol as it is drawn: its lines, arcs and outlines, and what it
    /// fills (a filled shape, a text, an image) - not the empty corners of
    /// its box, where a text is often put (an op-amp's name in its
    /// triangle's corner).
    QList<QLineF> strokes;
    QList<QPolygonF> solids;
    /// Whether it meets \a r (a wire: runs through it, its edges aside).
    bool meets(const QRect& r) const;
};

/// \a c's texts, each where it is drawn on the schematic: its name, then
/// each property shown for the simulator in the settings, one under the
/// other from its text's corner.
QList<QRect> textBoxes(const Component* c);
/// The same, each as tight as its letters are drawn (no line spacing, no
/// room after the last letter): what is tested for overlaps.
QList<QRect> inkBoxes(const Component* c);
/// All of them in one box (null with none shown).
QRect textBlock(const Component* c);
/// A net label's text as it is drawn, in its rounded box.
QRect labelBox(const WireLabel* l);

/// Everything of \a sch a text can be drawn over: its wires, its parts'
/// symbols and texts, its net labels, its diagrams as they are drawn
/// (title, axes' numbers and labels) and its paintings of text (a text, a
/// formula, a text box, a table, an image, a dimension).
QList<Thing> things(Schematic* sch);

/// A text drawn over something.
struct Overlap {
    QString message;   ///< "VEE's text overlaps the wire 340,260-340,420"
    QString part;      ///< the part whose text it is (empty for a net label)
    QRect box;         ///< the text's box
    QRect other;       ///< the box of what it overlaps
    QPoint where;      ///< the middle of where they meet
};

/// Each text of \a sch - a part's, a net label's - drawn over something:
/// once for each pair (two parts' texts over each other once).
QList<Overlap> overlaps(Schematic* sch);

/// Whether \a c's text is drawn over anything of \a all (its own text
/// aside).
bool textOverlaps(const Component* c, const QList<Thing>& all);
/// Likewise a net label's.
bool labelOverlaps(const WireLabel* l, const QList<Thing>& all);

/// A spot for \a c's text clear of everything in \a all (its own texts
/// left out of it): [tx, ty] from its centre, the nearest beside it -
/// tried at the right, the left, below and above, in that order where
/// they are as near - and \a side says which. None when none is within
/// a few grid steps.
std::optional<QPoint> freeSpot(const Component* c, const QList<Thing>& all, QString* side = nullptr);
/// A spot for net label \a l's text, clear of everything in \a all (its
/// own aside), near the point it names: [x, y] of the text's corner.
std::optional<QPoint> freeLabelSpot(const WireLabel* l, const QList<Thing>& all);

/// What \a c's text is drawn over of \a all, each named ("the wire
/// 340,260-340,420", "VEE's text"), each once.
QStringList overlapped(const Component* c, const QList<Thing>& all);

/// How diagram \a a ("it") is drawn over diagram \a b (number \a nb):
/// "it lies over diagram 2" (their frames), "its title runs into diagram
/// 2's x-axis label", "diagram 2's title runs into its x-axis label", "its
/// labels run into diagram 2's"; empty when they are apart. \a below gets
/// the y that would clear it when \a a is below \a b (else unchanged).
QString diagramClash(const ::Diagram* a, const ::Diagram* b, int nb, int* below = nullptr);

/// \a all without what \a owner is (a part's text and symbol, a label).
QList<Thing> without(const QList<Thing>& all, const void* owner, bool textOnly);

} // namespace qucs_s::textplace

#endif // QUCS_TEXTPLACEMENT_H
