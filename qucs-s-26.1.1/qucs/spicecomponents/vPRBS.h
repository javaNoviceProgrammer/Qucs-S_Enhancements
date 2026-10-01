/*
 * vPRBS.h - a pseudo-random bit sequence voltage source (ngspice's PRBS
 * and PAM4)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef VPRBS_H
#define VPRBS_H

#include "components/component.h"

/*!
 * \brief A voltage source sending a pseudo-random bit sequence: ngspice's
 *        PRBS(v1 v2 tbit td tr tf order seed), the bits of a maximal-length
 *        shift register of n stages (PRBS7, PRBS15, PRBS31 as a pattern
 *        generator sends them) - or, coded PAM4, the same register two bits
 *        a symbol, Gray coded on four levels (PRBS13Q, PRBS31Q).
 */
class vPRBS : public Component {
public:
  vPRBS();
  ~vPRBS() override;
  Component* newOne() override;
  static Element* info(QString&, char* &, bool getNewOne=false);
protected:
  QString netlist() override;
  QString spice_netlist(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault) override;
};

#endif // VPRBS_H
