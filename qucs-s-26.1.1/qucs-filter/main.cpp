/***************************************************************************
                          main.cpp  -  description
                             -------------------
    begin                : Thu Aug 28 18:17:41 CEST 2003
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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>

#include <QApplication>
#include <QString>
#include <QTranslator>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QDir>
#include <QFont>
#include <QSettings>

#include "qucsfilter.h"
#include "filter.h"
#include "tl_filter.h"
#include "../qucs/extsimkernels/spicecompat.h"
#include "../qucs/tooljson.h"

#include <QCoreApplication>
#include <QJsonArray>

struct tQucsSettings QucsSettings;

// #########################################################################
// --json: a spec on standard input, the schematic Calculate puts into the
// clipboard - and the order - on standard output. Nothing is shown, and
// no setting is read or written.
namespace {

namespace tj = qucs_s::tooljson;

// The attenuation, in dB, of a Butterworth or Chebyshev low-pass of order
// \a n with its -3 dB corner at 1, at the normalized frequency \a w (as
// LC_Filter makes them: a Chebyshev's corner is its 3 dB frequency).
double attenuationAt(int type, int n, double ripple, double w)
{
  if (type == TYPE_BUTTERWORTH) return 10.0 * log10(1.0 + pow(w, 2.0 * n));
  const double eps = sqrt(pow(10.0, ripple / 10.0) - 1.0);
  const double edge = 1.0 / cosh(acosh(1.0 / eps) / n);   // the ripple band's end
  const double x = w / edge;
  const double t = x <= 1.0 ? cos(n * acos(x)) : cosh(n * acosh(x));
  return 10.0 * log10(1.0 + eps * eps * t * t);
}

QJsonObject design(const QJsonObject& spec)
{
  auto error = [](const QString& why) { return QJsonObject{{QStringLiteral("error"), why}}; };
  const QString typeName = spec.value("type").toString(QStringLiteral("chebyshev")).toLower();
  static const QStringList types{"bessel", "butterworth", "chebyshev", "cauer"};
  int type = int(types.indexOf(typeName == QLatin1String("elliptic") ? QStringLiteral("cauer") : typeName));
  if (type < 0) return error(QStringLiteral("'type' is bessel, butterworth, chebyshev or cauer (elliptic), not %1").arg(typeName));
  const QString className = spec.value("class").toString(QStringLiteral("lowpass")).toLower().remove(QLatin1Char('_')).remove(QLatin1Char('-')).remove(QLatin1Char(' '));
  static const QStringList classes{"lowpass", "highpass", "bandpass", "bandstop"};
  const int fclass = int(classes.indexOf(className));
  if (fclass < 0) return error(QStringLiteral("'class' is lowpass, highpass, bandpass or bandstop, not %1").arg(className));
  // The window's list of realizations, by name.
  static const QStringList realizations{"lc_pi", "lc_tee", "c_coupled_lines", "microstrip_end_coupled", "coupled_lines",
                                        "microstrip_coupled", "stepped_impedance", "microstrip_stepped_impedance", "quarter_wave",
                                        "microstrip_quarter_wave", "equation"};
  QString realizeName = spec.value("realization").toString(QStringLiteral("lc_pi")).toLower();
  if (realizeName == QLatin1String("pi") || realizeName == QLatin1String("lc")) realizeName = QStringLiteral("lc_pi");
  if (realizeName == QLatin1String("tee") || realizeName == QLatin1String("t")) realizeName = QStringLiteral("lc_tee");
  const int realize = int(realizations.indexOf(realizeName));
  if (realize < 0) return error(QStringLiteral("'realization' is one of %1, not %2").arg(realizations.join(", "), realizeName));
  // As the window allows them: lines are band-pass, stepped impedance low-pass.
  if (realize >= 2 && realize < 6 && fclass != CLASS_BANDPASS)
    return error(QStringLiteral("%1 makes band-pass filters only").arg(realizeName));
  if ((realize == 6 || realize == 7) && fclass != CLASS_LOWPASS)
    return error(QStringLiteral("%1 makes low-pass filters only").arg(realizeName));
  if ((realize == 8 || realize == 9) && fclass != CLASS_BANDPASS && fclass != CLASS_BANDSTOP)
    return error(QStringLiteral("%1 makes band-pass and band-stop filters only").arg(realizeName));

  tFilter f;
  f.Type = type;
  f.Class = fclass;
  f.Impedance = spec.contains("impedance") ? tj::number(spec.value("impedance")) : 50.0;
  f.Ripple = spec.contains("ripple") ? tj::number(spec.value("ripple")) : 1.0;
  f.Attenuation = spec.contains("atten") ? tj::number(spec.value("atten")) : 20.0;
  f.Frequency = tj::number(spec.value(spec.contains("fc") ? "fc" : "f1"));
  f.Frequency2 = spec.contains("f2") ? tj::number(spec.value("f2")) : 0.0;
  f.Frequency3 = spec.contains("fs") ? tj::number(spec.value("fs")) : 0.0;
  f.Order = spec.contains("order") ? spec.value("order").toInt() : 0;
  if (!(f.Frequency > 0)) return error(QStringLiteral("'fc' is the corner frequency (band-pass, band-stop: the band's start) in Hz, above 0"));
  if (!(f.Impedance > 0)) return error(QStringLiteral("'impedance' is in ohms, above 0"));
  if ((fclass == CLASS_BANDPASS || fclass == CLASS_BANDSTOP) && !(f.Frequency2 > f.Frequency))
    return error(QStringLiteral("a band-pass or band-stop filter takes 'f2', the band's end, above 'fc'"));
  if ((type == TYPE_CHEBYSHEV || type == TYPE_CAUER) && !(f.Ripple > 0))
    return error(QStringLiteral("'ripple' is the pass band's ripple in dB, above 0"));

  QJsonObject result;
  if (type == TYPE_CAUER) {
    if (!(f.Frequency3 > 0) || !(f.Attenuation > 0))
      return error(QStringLiteral("a Cauer filter's order comes from 'atten' (dB) at 'fs' (Hz): give both"));
    if (spec.contains("order")) result.insert("note", "a Cauer filter's order comes from 'atten' at 'fs': the order given is not used");
  } else if (!spec.contains("order")) {
    // The lowest order that has 'atten' at 'fs' - a low-pass or high-pass.
    if (!(f.Frequency3 > 0) || !spec.contains("atten"))
      return error(QStringLiteral("give 'order', or 'atten' (dB) at 'fs' (Hz) to find it"));
    if (type == TYPE_BESSEL || fclass == CLASS_BANDPASS || fclass == CLASS_BANDSTOP)
      return error(QStringLiteral("the order is found from 'atten' at 'fs' for a Butterworth or Chebyshev low-pass or high-pass: give 'order'"));
    const double w = fclass == CLASS_LOWPASS ? f.Frequency3 / f.Frequency : f.Frequency / f.Frequency3;
    if (!(w > 1.0)) return error(QStringLiteral("'fs' is in the stop band: above 'fc' for a low-pass, below it for a high-pass"));
    f.Order = 0;
    for (int n = 2; n <= 19 && f.Order == 0; ++n)
      if (attenuationAt(type, n, f.Ripple, w) >= f.Attenuation) f.Order = n;
    if (f.Order == 0) return error(QStringLiteral("no order up to 19 has %1 dB at %2 Hz").arg(f.Attenuation).arg(f.Frequency3));
    result.insert("order from", QStringLiteral("%1 dB at %2 Hz: %3 dB there").arg(f.Attenuation).arg(f.Frequency3)
                                    .arg(attenuationAt(type, f.Order, f.Ripple, w), 0, 'f', 1));
  }
  if (type != TYPE_CAUER) {
    if (f.Order < 2) return error(QStringLiteral("the order is 2 or more"));
    if (f.Order > 19 && type == TYPE_BESSEL) return error(QStringLiteral("a Bessel filter's order is 19 at most"));
    if (f.Order > 40) return error(QStringLiteral("the order is 40 at most"));
  }

  tSubstrate sub;
  const QJsonObject s = spec.value("substrate").toObject();
  auto value = [&s](const char* key, double otherwise) { return s.contains(key) ? tj::number(s.value(key)) : otherwise; };
  sub.er = value("er", 9.8);
  sub.height = value("h", 1.0e-3);
  sub.thickness = value("t", 12.5e-6);
  sub.tand = 0.0;
  sub.resistivity = 1e-10;
  sub.roughness = 0.0;
  sub.minWidth = value("min_width", 0.4e-3);
  sub.maxWidth = value("max_width", 5.0e-3);

  QucsSettings.DefaultSimulator = tj::simulator(spec);
  QString* schematic = QucsFilter::schematicOf(&f, &sub, realize);
  if (schematic == nullptr) return error(QStringLiteral("the filter cannot be made with these values"));
  result.insert("schematic", *schematic);
  delete schematic;
  result.insert("order", f.Order);
  result.insert("type", types.at(type));
  result.insert("class", classes.at(fclass));
  result.insert("realization", realizations.at(realize));
  return result;
}

} // namespace



// #########################################################################
// Loads the settings file and stores the settings.
bool loadSettings()
{
    QSettings settings("qucs","qucs_s");
    settings.beginGroup("QucsFilter");
    if(settings.contains("x"))QucsSettings.x=settings.value("x").toInt();
    if(settings.contains("y"))QucsSettings.y=settings.value("y").toInt();
    settings.endGroup();
    if(settings.contains("Language"))QucsSettings.Language=settings.value("Language").toString();
    if(settings.contains("DefaultSimulator"))
        QucsSettings.DefaultSimulator = settings.value("DefaultSimulator").toInt();
    else QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;

  return true;
}


// #########################################################################
// Saves the settings in the settings file.
bool saveApplSettings(QucsFilter *qucs)
{
    QSettings settings ("qucs","qucs_s");
    settings.beginGroup("QucsFilter");
    settings.setValue("x", qucs->x());
    settings.setValue("y", qucs->y());
    settings.endGroup();
  return true;

}



// #########################################################################
// ##########                                                     ##########
// ##########                  Program Start                      ##########
// ##########                                                     ##########
// #########################################################################

int main(int argc, char *argv[])
{
  if (qucs_s::tooljson::wanted(argc, argv)) {
    QCoreApplication a(argc, argv);
    QString error;
    const QJsonObject spec = qucs_s::tooljson::spec(&error);
    if (!error.isEmpty()) return qucs_s::tooljson::fail(error);
    return qucs_s::tooljson::answer(design(spec));
  }
  QApplication a(argc, argv);

  // apply default settings
  QucsSettings.x = 200;
  QucsSettings.y = 100;

  // is application relocated?
  QDir QucsDir;
  QString QucsApplicationPath = QCoreApplication::applicationDirPath();
#ifdef __APPLE__
  QucsDir = QDir(QucsApplicationPath.section("/bin",0,0));
#else
  QucsDir = QDir(QucsApplicationPath);
  QucsDir.cdUp();
#endif
  QucsSettings.LangDir = QucsDir.canonicalPath() + "/share/" QUCS_NAME "/lang/";

  loadSettings();

  QTranslator tor( 0 );
  QString lang = QucsSettings.Language;
  if(lang.isEmpty())
    lang = QString(QLocale::system().name());
  static_cast<void>(tor.load( QStringLiteral("qucs_") + lang, QucsSettings.LangDir));
  a.installTranslator( &tor );

  QucsFilter *qucs = new QucsFilter();
  qucs->raise();
  qucs->move(QucsSettings.x, QucsSettings.y);  // position before "show" !!!
  qucs->show();
  int result = a.exec();
  saveApplSettings(qucs);
  return result;
}
