/***************************************************************************
                                 spicefile.h
                                -------------
    begin                : Tue Dec 28 2004
    copyright            : (C) 2004 by Michael Margraf
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

#ifndef SPICEFILE_H
#define SPICEFILE_H
#include "component.h"

#include <QObject>
#include <QDateTime>

class QProcess;
class QTextStream;
class QString;

class SpiceFile : public QObject, public MultiViewComponent  {
   Q_OBJECT
public:
  SpiceFile();
 ~SpiceFile() {};
  Component* newOne();
  static Element* info(QString&, char* &, bool getNewOne=false);

  bool withSim;
  bool createSubNetlist(QTextStream *);
  bool createSpiceSubckt(QTextStream * stream);
  QString getErrorText() { return ErrText; }
  QString getSubcircuitFile();
  /// The program and its arguments that preprocess \a file with
  /// \a preprocessor (ps2sp, spicepp, spiceprm): Perl running the script
  /// - the one beside Qucs-S, else found on PATH (perl -S) - on it, into
  /// \a output too for spiceprm. Empty for none, or one not known.
  static QStringList preprocessorCommand(const QString& preprocessor, const QString& file, const QString& output);

private:
  bool makeSubcircuit;
  bool insertSim;
  bool changed;
  QProcess *QucsConv, *SpicePrep;
  QString NetText, ErrText, NetLine, SimText;
  QTextStream *outstream, *filstream, *prestream;
  QDateTime lastLoaded;
  bool recreateSubNetlist(QString *, QString *);

protected:
  QString netlist();
  void createSymbol();
  QString spice_netlist(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault);
  virtual QString cdl_netlist();

private slots:
  void slotGetNetlist();
  void slotGetError();
  void slotExited();
  void slotSkipOut();
  void slotSkipErr();
  void slotGetPrepOut();
  void slotGetPrepErr();
};

#endif
