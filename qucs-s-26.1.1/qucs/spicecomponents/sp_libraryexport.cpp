/***************************************************************************
                             sp_libraryexport.cpp
                            ----------------------
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "sp_libraryexport.h"
#include "main.h"

#include <QFontInfo>
#include <QFontMetrics>

namespace {

// Its properties, in the order of the file.
const QString kAlwaysLoadOSDI = QStringLiteral("AlwaysLoadOSDI");
const QString kModelCards = QStringLiteral("ModelCards");
const QString kAlwaysModelCards = QStringLiteral("AlwaysModelCards");
const QString kGroundPin = QStringLiteral("GroundPin");

QString valueOf(const Component* c, const QString& name)
{
  for (const Property* p : c->Props)
    if (p->Name == name) return p->Value;
  return QString();
}

void setValue(Component* c, const QString& name, const QString& value)
{
  for (Property* p : c->Props)
    if (p->Name == name) {
      p->Value = value;
      return;
    }
}

// A yes/no property: yes as written in the file, or as one would type it.
bool yes(const QString& value)
{
  const QString v = value.trimmed().toLower();
  return v == QLatin1String("yes") || v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("on");
}

QString yesNo(bool value) { return value ? QStringLiteral("yes") : QStringLiteral("no"); }

} // namespace

LibraryExport::LibraryExport()
{
  isEquation = true;   // (nothing of a device: a SPICE netlist asks it for its expression, which is none)
  Type = isComponent;
  Description = QObject::tr("Library Export: what Project > Create Library does with this subcircuit - the settings of "
                            "its Document Settings > Library, held here and shown on the schematic. One a schematic; it "
                            "writes nothing into a netlist.");
  Simulator = spicecompat::simAll;   // (Create Library makes a SPICE model whichever simulator is in use)

  QFont f = QucsSettings.font;
  f.setWeight(QFont::Light);
  f.setPointSizeF(12.0);
  QFontMetrics metrics(f, 0);  // use the the screen-compatible metric
  const QString title = QObject::tr("Library Export");
  QSize r = metrics.size(0, title);
  int xb = r.width()  >> 1;
  int yb = r.height() >> 1;

  Lines.append(new qucs::Line(-xb, -yb, -xb,  yb, QPen(Qt::darkRed, 2)));
  Lines.append(new qucs::Line(-xb,  yb,  xb+3, yb, QPen(Qt::darkRed, 2)));
  Texts.append(new Text(-xb+4, -yb-3, title, QColor(0,0,0), QFontInfo(f).pixelSize()));

  x1 = -xb-3;  y1 = -yb-5;
  x2 =  xb+9;  y2 =  yb+3;

  tx = x1+4;
  ty = y2+4;

  Model = model();
  Name  = "LibExport";

  Props.append(new Property(kAlwaysLoadOSDI, QStringLiteral("no"), true,
                            QObject::tr("always load its Verilog-A (OSDI) in the project's circuits [no, yes]")));
  Props.append(new Property(kModelCards, QString(), true,
                            QObject::tr("SPICE .model cards of its own, written at the top level of a netlist, "
                                        "one a line (+ lines going on, * comments)")));
  Props.append(new Property(kAlwaysModelCards, QStringLiteral("no"), true,
                            QObject::tr("always write its .model cards in the project's circuits [no, yes]")));
  Props.append(new Property(kGroundPin, QStringLiteral("default"), true,
                            QObject::tr("a first pin gnd in its .SUBCKT - default: as Application Settings say "
                                        "[default, yes, no]")));
}

Component* LibraryExport::newOne()
{
  return new LibraryExport();
}

Element* LibraryExport::info(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("Library Export");
  BitmapFile = (char *) "sp_libexport";

  if (getNewOne) return new LibraryExport();
  return nullptr;
}

const QString& LibraryExport::model()
{
  static const QString name = QStringLiteral("LibraryExport");
  return name;
}

bool LibraryExport::is(const Component* c)
{
  return c != nullptr && c->Model == model();
}

LibrarySettings LibraryExport::settingsOf(const Component* c)
{
  LibrarySettings s;
  s.alwaysLoadOSDI = yes(valueOf(c, kAlwaysLoadOSDI));
  s.modelCards = valueOf(c, kModelCards).trimmed();
  s.alwaysModelCards = yes(valueOf(c, kAlwaysModelCards));
  const QString pin = valueOf(c, kGroundPin).trimmed().toLower();
  if (yes(pin) || pin == QLatin1String("with")) s.groundPin = LibrarySettings::With;
  else if (pin == QLatin1String("no") || pin == QLatin1String("0") || pin == QLatin1String("false") || pin == QLatin1String("off")
           || pin == QLatin1String("without"))
    s.groundPin = LibrarySettings::Without;
  return s;
}

void LibraryExport::setSettings(Component* c, const LibrarySettings& s)
{
  setValue(c, kAlwaysLoadOSDI, yesNo(s.alwaysLoadOSDI));
  setValue(c, kModelCards, s.modelCards.trimmed());
  setValue(c, kAlwaysModelCards, yesNo(s.alwaysModelCards));
  setValue(c, kGroundPin, s.groundPin == LibrarySettings::Default ? QStringLiteral("default") : yesNo(s.groundPin == LibrarySettings::With));
}
