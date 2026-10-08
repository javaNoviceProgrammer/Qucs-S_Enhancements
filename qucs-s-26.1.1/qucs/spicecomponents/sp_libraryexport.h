/***************************************************************************
                              sp_libraryexport.h
                             --------------------
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef SP_LIBRARYEXPORT_H
#define SP_LIBRARYEXPORT_H

#include "components/component.h"
#include "librarysettings.h"

/// Library Export (SPICE subcircuit in the components): placed on a
/// subcircuit's schematic, it holds what Project > Create Library does with
/// the subcircuit - the settings of its Document Settings > Library, shown
/// on the schematic. One a schematic; it writes nothing into a netlist.
class LibraryExport : public Component {
public:
  LibraryExport();
  ~LibraryExport() override = default;
  Component* newOne() override;
  static Element* info(QString&, char* &, bool getNewOne = false);

  /// Its type's name (Model) - "LibraryExport".
  static const QString& model();
  /// Whether \a c is a Library Export.
  static bool is(const Component* c);
  /// The settings \a c holds (its properties read; an odd value as its default).
  static LibrarySettings settingsOf(const Component* c);
  /// Gives \a c the settings \a s (its properties written).
  static void setSettings(Component* c, const LibrarySettings& s);

protected:
  // Nothing in any netlist: what it holds is Create Library's.
  QString netlist() override { return QString(); }
  QString spice_netlist(spicecompat::SpiceDialect) override { return QString(); }
  QString cdl_netlist() override { return QString(); }
};

#endif
