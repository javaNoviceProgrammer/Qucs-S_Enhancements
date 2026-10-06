/***************************************************************************
                               libcomp.h
                              -----------
    begin                : Fri Jun 10 2005
    copyright            : (C) 2005 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef LIBCOMP_H
#define LIBCOMP_H

#include "component.h"

class QTextStream;
class QString;


class LibComp : public MultiViewComponent  {
public:
  LibComp();
 ~LibComp() {};
  Component* newOne();

  bool createSubNetlist(QTextStream *, QStringList&, int type=1);
  /// Its library: libraryFileOf() its Lib and Comp, for the schematic it
  /// is in. The part's model and the files of the library's folder
  /// (getSubcircuitFile()) come from that one file.
  QString libraryFile() const;
  /// The library of a part whose Lib is \a lib in a schematic in \a folder
  /// (its file's folder; empty: in none) that has the component \a comp:
  /// the file at its path (a path it names; the installed library of a
  /// name); else, of the libraries of its name where misc::properAbsFileName()
  /// looks (librariesNamed()), the one the project \a project (none: the
  /// open one) or the schematic's folder linked its Verilog-A from
  /// (projectlibraries::linkedFolders()) - a library of the name found
  /// earlier, made later, does not take the part -, else the first. No
  /// \a comp: the first there is.
  static QString libraryFileOf(const QString& lib, const QString& folder, const QString& comp = QString(),
                               const QString& project = QString());
  /// Every library a part whose Lib is \a lib in a schematic in \a folder
  /// names, in the order libraryFileOf() takes them; their real paths.
  static QStringList librariesNamed(const QString& lib, const QString& folder);
  /// The other libraries of the name of \a libraryFile a part placed by
  /// that name finds (librariesNamed() for a schematic of the open project),
  /// in that order - a part of a name two of them have is taken from the
  /// first. Said when a library is made or brought in.
  static QStringList librariesNamedLike(const QString& libraryFile);
  /// Whether the library \a libraryFile has a component \a comp (its
  /// components read once while the file is unchanged).
  static bool hasComponent(const QString& libraryFile, const QString& comp);
  /// Whether a part of the component \a comp of \a libraryFile, of \a pins
  /// pins, ties a first pin of its SPICE model to the circuit's ground: its
  /// .SUBCKT has a pin more than the part, gnd - as Create Library writes it
  /// when the settings ask for it (LibraryGroundPin), and always did before
  /// 26.1.6 -, or the library has no SPICE model and its Qucs model is made
  /// one (qucs2spice gives that a gnd pin). False when the .SUBCKT has the
  /// part's pins and no more; true, as before, for a count that fits neither.
  static bool takesGround(const QString& libraryFile, const QString& comp, int pins);
  /// The SPICE subcircuit a part of the component \a comp of the library
  /// its Lib \a lib names stands for: LIB_COMP, as a name (createType()).
  static QString subcircuitName(const QString& lib, const QString& comp);
  /// The components of \a libraryFile marked <AlwaysLoadOSDI> - their
  /// subcircuit's Document Settings asked for it -, whose Verilog-A every
  /// circuit of a project that has the library loads, placed or not
  /// (projectlibraries::alwaysLoadedModules()). In the library's order.
  static QStringList alwaysLoaded(const QString& libraryFile);
  /// What a part's Lib holds to name the library \a libraryFile: its name
  /// when the name finds this very library (libraryFileOf() with no
  /// schematic) - a schematic that goes to another computer, or a library
  /// that moves, still finds it -, else its path without ".lib" (another
  /// library of its name is found first: installed, the project's,
  /// user_lib's, an earlier search path's).
  static QString referenceTo(const QString& libraryFile);
  /// The library's folder: libraryFile() without ".lib".
  QString getSubcircuitFile();
  QString getSpiceLibrary();
  /// The .va files attached to the component in its library (Create
  /// Library embeds them), in the library's folder, and the .osdi model
  /// compiled from each where there is one (OpenVAF puts it beside the
  /// source).
  QStringList getVerilogAFiles() override;
  /// getVerilogAFiles() of the component \a comp of the library \a libraryFile.
  static QStringList verilogAFilesOf(const QString& libraryFile, const QString& comp);
  /// The part's model when it is one component line (a varactor's
  /// <Diode ...> with the library's values, a MOSFET's <_MOSFET ...>): the
  /// library panel places that component, not a Lib - whose netlist would
  /// read the diode's values as its library and part, or name a subcircuit
  /// there is none of under the library's default symbol. Empty for any
  /// other part.
  QString componentModel();
  /// The part's description in its library ("68W audio amplifier"); empty
  /// when it has none or the library is not found.
  QString description();

protected:
  QString netlist();
  QString spice_netlist(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault);
  virtual QString cdl_netlist();
  QString vhdlCode(int);
  QString verilogCode(int);
  void createSymbol();

private:
  int  loadSymbol();
  /// Its pins named as its model names them - _netC and _netA of an LED
  /// (C and A), _netP_INN of an op-amp's (INN, and INP of _netN_INP),
  /// through the subcircuit
  /// the model wraps - when every pin gets a name of its own; a name the
  /// symbol gives a pin stays. connect takes U1.inn then, and a part put in
  /// another's place takes the pins by name.
  void namePinsFromModel();
  int  loadSection(const QString&, QString&, QStringList* i=0, QStringList *Attach=0);
  /// loadSection() of the component \a comp of the library \a libraryFile.
  static int loadSectionOf(const QString& libraryFile, const QString& comp, const QString& Name, QString& Section,
                           QStringList* Includes, QStringList* Attach);
  QString createType();
};

#endif
