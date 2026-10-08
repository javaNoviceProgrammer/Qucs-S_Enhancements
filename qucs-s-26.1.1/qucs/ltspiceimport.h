/*
 * ltspiceimport.h - an LTspice schematic (.asc) as a SPICE netlist: its
 * parts, the nets its wires, flags and pins make, its directives
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LTSPICEIMPORT_H
#define QUCS_LTSPICEIMPORT_H

#include <QByteArray>
#include <QPoint>
#include <QString>
#include <QStringList>

/*!
 * An .asc is a drawing: SYMBOLs placed and turned (R0 ... M270), WIREs,
 * FLAGs naming nets (0: ground), TEXTs - directives (!.tran 1m) and
 * comments. A part's pins are where its symbol's are, turned with it: read
 * from its .asy when one is found (beside the .asc, or in a folder given -
 * LTspice's lib/sym), else from LTspice's standard symbols as known here
 * (res, cap, ind, voltage, current, the diodes, the bipolars, the MOSFETs,
 * bv, bi). Wires join where they end - on each other, on a pin, on a
 * flag, or part way along another - as LTspice joins them.
 */
namespace qucs_s::ltspice {

struct Conversion {
    QString netlist;     ///< SPICE: its elements, then its directives, .end
    QStringList notes;   ///< what was left out or changed, and why
    QString error;       ///< why there is none
    int parts = 0;
    int nets = 0;
};

/// \a asc's text as a netlist; \a name says what it was (in its first line).
Conversion convert(const QString& asc, const QStringList& symbolFolders = {}, const QString& name = QString());

/// The text of an .asc as LTspice wrote it: UTF-16 (LTspice 24, with or
/// without its mark), else 8 bits - UTF-8, or Windows' Latin-1 (its µ).
QString textOf(const QByteArray& bytes);

/// Whether \a text is an LTspice schematic's ("Version 4", "SHEET ...").
bool looksLikeAsc(const QString& text);

/// Where an LTspice turn puts a symbol's point \a p (R90: a quarter turn
/// clockwise on the screen, y down; M: mirrored left to right first).
QPoint turned(const QPoint& p, const QString& rotation);

} // namespace qucs_s::ltspice

#endif
