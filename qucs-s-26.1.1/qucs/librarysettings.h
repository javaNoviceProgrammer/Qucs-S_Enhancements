/***************************************************************************
                              librarysettings.h
                             -------------------
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef LIBRARYSETTINGS_H
#define LIBRARYSETTINGS_H

#include <QString>

/// What a subcircuit asks of Project > Create Library when it is made into
/// a library part: held by the Library Export part placed on its schematic
/// (SPICE subcircuit in the components), or else by its Document Settings >
/// Library - one place at a time (Schematic::librarySettings()).
struct LibrarySettings {
  /// Its Verilog-A models (OSDI) loaded in every circuit of a project that
  /// has the library, placed or not (<AlwaysLoadOSDI> in the library).
  bool alwaysLoadOSDI = false;
  /// SPICE .model cards of its own, written at the top level of a netlist
  /// (after its .ENDS): one card a line, + lines going on, * comments.
  QString modelCards;
  /// Those cards in every circuit of a project that has the library, placed
  /// or not (<AlwaysModelCards> in the library).
  bool alwaysModelCards = false;
  /// Its .SUBCKT's first pin gnd: Default as Application Settings say
  /// (Ground pin (gnd) in exported subcircuits), or this subcircuit's own.
  enum GroundPin { Default = -1, Without = 0, With = 1 };
  GroundPin groundPin = Default;

  bool operator==(const LibrarySettings&) const = default;
  bool isDefault() const { return *this == LibrarySettings{}; }
  /// Whether its .SUBCKT has the pin gnd, \a application the setting's choice.
  bool groundPinFor(bool application) const { return groundPin == Default ? application : groundPin == With; }
};

#endif
