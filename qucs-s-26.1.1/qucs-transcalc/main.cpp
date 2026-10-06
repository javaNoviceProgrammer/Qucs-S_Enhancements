/***************************************************************************
                          main.cpp  -  description
                             -------------------
    begin                : Sun Feb 27 2005
    copyright            : (C) 2005 by Stefan Jahn
    email                : stefan@lkcc.org
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

#include "qucstrans.h"
#include "../qucs/tooljson.h"

tQucsSettings QucsSettings;

extern struct TransUnit TransUnits[];


// #########################################################################
// Loads the settings file and stores the settings.
bool loadSettings()
{
    QSettings settings("qucs","qucs_s");
    settings.beginGroup("QucsTranscalc");
    if(settings.contains("x"))QucsSettings.x=settings.value("x").toInt();
    if(settings.contains("y"))QucsSettings.y=settings.value("y").toInt();
    if(settings.contains("dx"))QucsSettings.dx=settings.value("dx").toInt();
    if(settings.contains("dy"))QucsSettings.dy=settings.value("dy").toInt();
    if(settings.contains("Mode"))QucsSettings.Mode=settings.value("Mode").toString();
    if(settings.contains("FreqUnit"))QucsSettings.freq_unit=settings.value("FreqUnit").toInt();
    if(settings.contains("LengthUnit"))QucsSettings.length_unit=settings.value("LengthUnit").toInt();
    if(settings.contains("ResUnit"))QucsSettings.res_unit=settings.value("ResUnit").toInt();
    if(settings.contains("AngUnit"))QucsSettings.ang_unit=settings.value("AngUnit").toInt();

    settings.endGroup();
    if(settings.contains("font"))QucsSettings.font.fromString(settings.value("font").toString());
    if(settings.contains("Language"))QucsSettings.Language=settings.value("Language").toString();
    if(settings.contains("QucsHomeDir"))
      if(settings.value("QucsHomeDir").toString() != "")
	QucsSettings.QucsHomeDir.setPath(settings.value("QucsHomeDir").toString());

    QucsSettings.QucsWorkDir = QucsSettings.QucsHomeDir;

  return true;
}


// #########################################################################
// Saves the settings in the settings file.
bool saveApplSettings(QucsTranscalc *qucs)
{
    QSettings settings ("qucs","qucs_s");
    settings.beginGroup("QucsTranscalc");
    settings.setValue("x", qucs->x());
    settings.setValue("y", qucs->y());
    settings.setValue("dx", qucs->width());
    settings.setValue("dy", qucs->height());
    settings.setValue("Mode", qucs->getMode());
    settings.setValue("FreqUnit", TransUnits[0].units[QucsSettings.freq_unit]);
    settings.setValue("LengthUnit", TransUnits[1].units[QucsSettings.length_unit]);
    settings.setValue("ResUnit", TransUnits[2].units[QucsSettings.res_unit]);
    settings.setValue("AngUnit", TransUnits[3].units[QucsSettings.ang_unit]);
    settings.endGroup();

    return true;
}


// #########################################################################
// ##########                                                     ##########
// ##########                  Program Start                      ##########
// ##########                                                     ##########
// #########################################################################
// --json: a spec on standard input; the window, made but not shown, is
// filled in from it (in metres, hertz, ohms and degrees) and Analyze or
// Synthesize pressed - every value and the results on standard output.
// No setting or file is read or written.
namespace {

namespace tj = qucs_s::tooljson;

QJsonObject calculate(QucsTranscalc& w, const QJsonObject& spec)
{
  auto error = [](const QString& why) { return QJsonObject{{QStringLiteral("error"), why}}; };
  static const QStringList types{"microstrip", "coplanar", "grounded_coplanar", "rectangular", "coaxial", "coupled_microstrip", "stripline"};
  static const char* modes[] = {"Microstrip", "Coplanar", "GroundedCoplanar", "Rectangular", "Coaxial", "CoupledMicrostrip", "Stripline"};
  QString type = spec.value("type").toString(QStringLiteral("microstrip")).toLower().replace(QLatin1Char(' '), QLatin1Char('_')).replace(QLatin1Char('-'), QLatin1Char('_'));
  if (type == QLatin1String("coax")) type = QStringLiteral("coaxial");
  if (type == QLatin1String("waveguide") || type == QLatin1String("rectangular_waveguide")) type = QStringLiteral("rectangular");
  if (type == QLatin1String("coupled")) type = QStringLiteral("coupled_microstrip");
  const int t = int(types.indexOf(type));
  if (t < 0) return error(QStringLiteral("'type' is one of %1, not %2").arg(types.join(", "), type));
  const QString job = spec.value("do").toString(spec.contains("z0") || spec.contains("z0e") ? "synthesize" : "analyze").toLower();
  if (job != QLatin1String("analyze") && job != QLatin1String("synthesize"))
    return error(QStringLiteral("'do' is analyze (the geometry gives Z0) or synthesize (Z0 gives the geometry)"));
  w.setMode(QString::fromLatin1(modes[t]));
  // Every value in metres, hertz, ohms and degrees.
  const QStringList names = w.propertyNames();
  for (const QString& n : names) {
    const QStringList units = w.unitsOf(n);
    const QString kind = units.value(0);
    const char* unit = kind == QLatin1String("mil") ? "m" : kind == QLatin1String("GHz") ? "Hz" : kind == QLatin1String("Ohm") ? "Ohm"
                     : kind == QLatin1String("Deg") ? "Deg" : nullptr;
    if (unit) w.setUnit(n, unit);
  }
  // The spec's values by name, case aside ("er", "h", "w", "z0", "f").
  QStringList unknown;
  for (auto it = spec.constBegin(); it != spec.constEnd(); ++it) {
    static const QStringList own{"type", "do", "solve_for", "simulator"};
    if (own.contains(it.key())) continue;
    if (it.key() == QLatin1String("substrate") && it.value().isObject()) {
      const QJsonObject sub = it.value().toObject();
      for (auto jt = sub.constBegin(); jt != sub.constEnd(); ++jt) {
        QString name;
        for (const QString& n : names)
          if (n.compare(jt.key(), Qt::CaseInsensitive) == 0) name = n;
        if (name.isEmpty()) unknown << jt.key();
        else w.setProperty(name, tj::number(jt.value()));
      }
      continue;
    }
    QString key = it.key();
    if (key == QLatin1String("f") || key == QLatin1String("frequency")) key = QStringLiteral("Freq");
    if (key == QLatin1String("angle") || key == QLatin1String("electrical_length")) key = QStringLiteral("Ang_l");
    QString name;
    for (const QString& n : names)
      if (n.compare(key, Qt::CaseInsensitive) == 0) name = n;
    if (name.isEmpty()) unknown << it.key();
    else w.setProperty(name, tj::number(it.value()));
  }
  if (!unknown.isEmpty())
    return error(QStringLiteral("a %1 has no %2: its values are %3").arg(type, unknown.join(", "), names.join(", ")));
  if (spec.contains("solve_for") && !w.solveFor(spec.value("solve_for").toString()))
    return error(QStringLiteral("'solve_for' names a physical value synthesis may solve for (of a %1: one with a choice in the window)").arg(type));
  const int status = job == QLatin1String("analyze") ? w.analyze() : w.synthesize();
  QJsonObject values;
  for (const QString& n : names) values.insert(n, w.getProperty(n));
  QJsonObject results;
  for (const auto& [name, value] : w.results()) results.insert(name, value);
  QJsonObject r{{"type", type}, {"did", job}, {"values", values}, {"results", results},
                {"units", "lengths m, frequency Hz, impedances ohm, angles degrees"}};
  if (status != 0) r.insert("error", QStringLiteral("%1 did not converge").arg(job));
  return r;
}

} // namespace

int main(int argc, char *argv[])
{
  if (qucs_s::tooljson::wanted(argc, argv)) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication a(argc, argv);
    QString error;
    const QJsonObject spec = qucs_s::tooljson::spec(&error);
    if (!error.isEmpty()) return qucs_s::tooljson::fail(error);
    QucsSettings.font = QFont("Helvetica", 12);
    QucsTranscalc w;
    return qucs_s::tooljson::answer(calculate(w, spec));
  }
  QApplication a(argc, argv);

  // apply default settings
  QucsSettings.x = 100;
  QucsSettings.y = 50;
  QucsSettings.dx = 540;
  QucsSettings.dy = 400;
  QucsSettings.font = QFont("Helvetica", 12);
  QucsSettings.length_unit = 0;
  QucsSettings.res_unit = 0;
  QucsSettings.ang_unit = 0;
  QucsSettings.freq_unit = 0;
  QucsSettings.QucsHomeDir.setPath(QDir::homePath() + "/QucsWorkspace");

  // is application relocated?
  char * var = getenv ("QUCSDIR");
  QDir QucsDir;
  if (var != NULL) {
    QucsDir = QDir (var);
    QString QucsDirStr = QucsDir.canonicalPath ();
    QucsSettings.LangDir =
      QDir::toNativeSeparators(QucsDirStr + "/share/qucs/lang/");
  } else {
    QString QucsApplicationPath = QCoreApplication::applicationDirPath();
#ifdef __APPLE__
    QucsDir = QDir(QucsApplicationPath.section("/bin",0,0));
#else
    QucsDir = QDir(QucsApplicationPath);
    QucsDir.cdUp();
#endif
    QucsSettings.LangDir = QucsDir.canonicalPath() + "/share/qucs/lang/";
  }
  loadSettings();

  a.setFont(QucsSettings.font);

  QTranslator tor( 0 );
  QString lang = QucsSettings.Language;
  if(lang.isEmpty())
    lang = QString(QLocale::system().name());
  static_cast<void>(tor.load( QStringLiteral("qucs_") + lang, QucsSettings.LangDir));
  a.installTranslator( &tor );

  QucsTranscalc *qucs = new QucsTranscalc();
  qucs->raise();
  qucs->resize(QucsSettings.dx, QucsSettings.dy); // size and position ...
  qucs->move(QucsSettings.x, QucsSettings.y);     // ... before "show" !!!
  qucs->show();

  // load file with all the GUI input values from the Qucs Home
  qucs->loadFile(QucsSettings.QucsHomeDir.filePath("transrc"));
  qucs->setMode(QucsSettings.Mode);

  // optional file argument
  if (argc > 1) {
    int _mode = 0;
    QString File = argv[1];
    qucs->loadFile(File,&_mode);
  }

  int result = a.exec();
  saveApplSettings(qucs);
  // save file with all the GUI input values in the Qucs Home
  qucs->saveModes(QucsSettings.QucsHomeDir.filePath("transrc"));
  delete qucs;
  return result;
}
