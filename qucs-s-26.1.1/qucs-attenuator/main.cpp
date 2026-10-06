/****************************************************************************
**     Qucs Attenuator Synthesis
**     main.cpp
**
**
**
**
**
**
**
*****************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
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

#include "qucsattenuator.h"
#include "attenuatorfunc.h"
#include "../qucs/tooljson.h"

#include <QJsonArray>

struct tQucsSettings QucsSettings;

// #########################################################################
// --json: a spec on standard input, the resistors and the schematic the
// window puts into the clipboard on standard output. Nothing is shown, and
// no setting is read or written.
namespace {

namespace tj = qucs_s::tooljson;

QJsonObject design(const QJsonObject& spec)
{
    auto error = [](const QString& why) { return QJsonObject{{QStringLiteral("error"), why}}; };
    // The window's list, by name.
    static const QStringList topologies{"pi", "tee", "bridged_tee", "reflection", "quarter_wave_series", "quarter_wave_shunt",
                                        "l_pad_series_first", "l_pad_shunt_first", "series", "shunt"};
    const QString topology = spec.value("topology").toString(QStringLiteral("pi")).toLower().replace(QLatin1Char(' '), QLatin1Char('_')).replace(QLatin1Char('-'), QLatin1Char('_'));
    const int t = int(topologies.indexOf(topology == QLatin1String("t") ? QStringLiteral("tee") : topology));
    if (t < 0) return error(QStringLiteral("'topology' is one of %1, not %2").arg(topologies.join(", "), topology));
    tagATT a{};
    a.Topology = t;
    a.Attenuation = tj::number(spec.value("attenuation"));
    a.Zin = spec.contains("z_in") ? tj::number(spec.value("z_in")) : 50.0;
    a.Zout = spec.contains("z_out") ? tj::number(spec.value("z_out")) : a.Zin;
    a.minR = spec.value("r_below_z0").toBool(true);
    a.freq = spec.contains("f") ? tj::number(spec.value("f")) : 1e9;
    a.useLumped = spec.value("lumped").toBool(false);
    a.Pin = spec.contains("p_in") ? tj::number(spec.value("p_in")) : 1e-3;
    if (!(a.Attenuation > 0)) return error(QStringLiteral("'attenuation' is in dB, above 0"));
    if (!(a.Zin > 0) || !(a.Zout > 0)) return error(QStringLiteral("'z_in' and 'z_out' are in ohms, above 0"));
    if ((t == 4 || t == 5) && !(a.freq > 0)) return error(QStringLiteral("a quarter-wave attenuator takes 'f', its frequency in Hz"));
    QUCS_Att att;
    if (att.Calc(&a) == -1)
        return error(QStringLiteral("a %1 attenuator from %2 to %3 ohms is %4 dB at least").arg(topology).arg(a.Zin).arg(a.Zout)
                         .arg(a.MinimumATT, 0, 'f', 3));
    QucsSettings.DefaultSimulator = tj::simulator(spec);
    QString* s = QUCS_Att::createSchematic(&a, spec.value("s_parameters").toBool(true));
    if (s == nullptr) return error(QStringLiteral("the attenuator cannot be made with these values"));
    QJsonObject r{{"schematic", *s}, {"topology", topologies.at(t)}};
    delete s;
    QJsonObject values, power;
    const double rs[] = {a.R1, a.R2, a.R3, a.R4}, ps[] = {a.PR1, a.PR2, a.PR3, a.PR4};
    for (int i = 0; i < 4; ++i)
        if (rs[i] > 0) {
            values.insert(QStringLiteral("R%1").arg(i + 1), rs[i]);
            power.insert(QStringLiteral("R%1").arg(i + 1), ps[i]);
        }
    r.insert("ohms", values);
    r.insert("watts dissipated", power);
    if (t == 4 || t == 5) r.insert("quarter wave length (m)", a.L);
    return r;
}

} // namespace

// #########################################################################
// Loads the settings file and stores the settings.
bool loadSettings()
{
    QSettings settings("qucs","qucs_s");
    settings.beginGroup("QucsAttenuator");
    if(settings.contains("x"))QucsSettings.x=settings.value("x").toInt();
    if(settings.contains("y"))QucsSettings.y=settings.value("y").toInt();
    settings.endGroup();
    if(settings.contains("font"))QucsSettings.font.fromString(settings.value("font").toString());
    if(settings.contains("Language"))QucsSettings.Language=settings.value("Language").toString();
    // (Qucs-S's: a schematic's equations as that simulator reads them.)
    if(settings.contains("DefaultSimulator")) QucsSettings.DefaultSimulator = settings.value("DefaultSimulator").toInt();

  return true;
}


// #########################################################################
// Saves the settings in the settings file.
bool saveApplSettings(QucsAttenuator *qucs)
{
    QSettings settings ("qucs","qucs_s");
    settings.beginGroup("QucsAttenuator");
    settings.setValue("x", qucs->x());
    settings.setValue("y", qucs->y());
    settings.endGroup();
  return true;

}



int main( int argc, char ** argv )
{
  if (qucs_s::tooljson::wanted(argc, argv)) {
    QCoreApplication a(argc, argv);
    QString error;
    const QJsonObject spec = qucs_s::tooljson::spec(&error);
    if (!error.isEmpty()) return qucs_s::tooljson::fail(error);
    return qucs_s::tooljson::answer(design(spec));
  }
  QApplication a( argc, argv );

  // apply default settings
  QucsSettings.x = 200;
  QucsSettings.y = 100;

  // is application relocated?
  char * var = getenv ("QUCSDIR");
  QDir QucsDir;
  if (var != NULL) {
    QucsDir = QDir (var);
    QString QucsDirStr = QucsDir.canonicalPath ();
    QucsSettings.LangDir =
      QDir::toNativeSeparators (QucsDirStr + "/share/" QUCS_NAME "/lang/");
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

  QTranslator tor( 0 );
  QString lang = QucsSettings.Language;
  if(lang.isEmpty())
    lang = QString(QLocale::system().name());
  static_cast<void>(tor.load( QStringLiteral("qucs_") + lang, QucsSettings.LangDir));
  a.installTranslator( &tor );

  QucsAttenuator *qucs = new QucsAttenuator();
  //a.setMainWidget(qucs);
  qucs->raise();
  qucs->move(QucsSettings.x, QucsSettings.y);  // position before "show" !!!
  qucs->show();
  int result = a.exec();
  saveApplSettings(qucs);
  return result;

  //  QApplication a( argc, argv );
  //  QucsAttenuator w;
  //  w.show();
  //  a.connect( &a, SIGNAL( lastWindowClosed() ), &a, SLOT( quit() ) );
  //  return a.exec();
}
