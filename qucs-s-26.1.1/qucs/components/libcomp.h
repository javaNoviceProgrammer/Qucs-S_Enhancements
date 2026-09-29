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
  QString getSubcircuitFile();
  QString getSpiceLibrary();
  /// The .va files attached to the component in its library (Create
  /// Library embeds them), in the library's folder, and the .osdi model
  /// compiled from each where there is one (OpenVAF puts it beside the
  /// source).
  QStringList getVerilogAFiles() override;
  /// The part's model when it is one component line (a varactor's
  /// <Diode ...> with the library's values, a MOSFET's <_MOSFET ...>): the
  /// library panel places that component, not a Lib - whose netlist would
  /// read the diode's values as its library and part, or name a subcircuit
  /// there is none of under the library's default symbol. Empty for any
  /// other part.
  QString componentModel();

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
  QString createType();
};

#endif
