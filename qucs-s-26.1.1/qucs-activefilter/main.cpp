/***************************************************************************
                                 main.cpp
                              ----------------
    begin                : Wed Apr 10 2014
    copyright            : (C) 2014 by Vadim Kuznetsov
    email                : ra3xdh@gmail.com
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
#include <QtCore>
#include <QtWidgets> 
#include <QSvgWidget> 
#include <QApplication>

#include "qucsactivefilter.h"
#include "filter.h"
#include "sallenkey.h"
#include "mfbfilter.h"
#include "schcauer.h"
#include "../qucs/tooljson.h"

#include <QJsonArray>

struct tQucsSettings QucsSettings;

// #########################################################################
// --json: a spec on standard input, the schematic "Calculate and copy to
// clipboard" makes - with its parts and order - on standard output.
// Nothing is shown, and no setting is read or written.
namespace {

namespace tj = qucs_s::tooljson;

QJsonObject design(const QJsonObject& spec)
{
    auto error = [](const QString& why) { return QJsonObject{{QStringLiteral("error"), why}}; };
    static const QStringList responses{"butterworth", "chebyshev", "inverse_chebyshev", "cauer", "bessel", "legendre"};
    QString response = spec.value("type").toString(QStringLiteral("butterworth")).toLower();
    if (response == QLatin1String("elliptic")) response = QStringLiteral("cauer");
    const Filter::FilterFunc funcs[] = {Filter::Butterworth, Filter::Chebyshev, Filter::InvChebyshev, Filter::Cauer, Filter::Bessel, Filter::Legendre};
    const int r = int(responses.indexOf(response));
    if (r < 0) return error(QStringLiteral("'type' is one of %1, not %2").arg(responses.join(", "), response));
    const Filter::FilterFunc ffunc = funcs[r];
    static const QStringList classes{"lowpass", "highpass", "bandpass", "bandstop"};
    const QString className = spec.value("class").toString(QStringLiteral("lowpass")).toLower().remove(QLatin1Char('_')).remove(QLatin1Char('-')).remove(QLatin1Char(' '));
    const Filter::FType types[] = {Filter::LowPass, Filter::HighPass, Filter::BandPass, Filter::BandStop};
    const int c = int(classes.indexOf(className));
    if (c < 0) return error(QStringLiteral("'class' is lowpass, highpass, bandpass or bandstop, not %1").arg(className));
    const Filter::FType ftyp = types[c];
    static const QStringList topologies{"sallen_key", "mfb", "cauer"};
    const QString topology = spec.value("realization").toString(QStringLiteral("sallen_key")).toLower();
    const int t = int(topologies.indexOf(topology));
    if (t < 0) return error(QStringLiteral("'realization' is sallen_key, mfb (multiple feedback) or cauer, not %1").arg(topology));

    FilterParam par{};
    par.As = spec.contains("atten") ? tj::number(spec.value("atten")) : 20.0;
    par.Rp = spec.contains("ripple") ? tj::number(spec.value("ripple")) : 1.0;
    par.Kv = pow(10.0, (spec.contains("gain") ? tj::number(spec.value("gain")) : 0.0) / 20.0);
    par.order = spec.value("order").toInt();
    if (ftyp == Filter::LowPass || ftyp == Filter::HighPass) {
        par.Ap = spec.contains("ap") ? tj::number(spec.value("ap")) : 3.0;
        par.Fc = tj::number(spec.value("fc"));
        par.Fs = tj::number(spec.value("fs"));
        if (!(par.Fc > 0)) return error(QStringLiteral("'fc' is the cutoff frequency in Hz"));
        if (!(par.Fs > 0) && ffunc != Filter::Bessel && ffunc != Filter::Legendre)
            return error(QStringLiteral("'fs' is the stop band's frequency in Hz, where 'atten' is reached: it gives the order"));
        if (ffunc == Filter::Bessel || ffunc == Filter::Legendre) {
            // (Their order is given: a stop band frequency the other side
            // of the cutoff keeps the window's checks quiet.)
            if (!(par.Fs > 0)) par.Fs = ftyp == Filter::LowPass ? 2.0 * par.Fc : par.Fc / 2.0;
        }
    } else {
        par.Fl = tj::number(spec.value("fc"));
        par.Fu = tj::number(spec.value("f2"));
        par.TW = tj::number(spec.value("transition"));
        if (!(par.Fl > 0) || !(par.Fu > par.Fl))
            return error(QStringLiteral("a band-pass or band-stop filter is from 'fc' to 'f2' (Hz), 'f2' above 'fc'"));
        if (!(par.TW > 0)) return error(QStringLiteral("'transition' is the width in Hz from each band edge to where 'atten' is reached"));
    }
    if ((ffunc == Filter::Bessel || ffunc == Filter::Legendre) && par.order < 1)
        return error(QStringLiteral("a %1 filter's order is given: 'order'").arg(response));
    if (t == 1 && (ffunc == Filter::InvChebyshev || ffunc == Filter::Cauer))
        return error(QStringLiteral("a multiple feedback filter cannot have a Cauer or inverse Chebyshev response: use cauer"));
    if (t == 2 && !(ffunc == Filter::InvChebyshev || ffunc == Filter::Cauer || ftyp == Filter::BandStop))
        return error(QStringLiteral("a Cauer section is for a Cauer or inverse Chebyshev response, or a band-stop filter"));

    QString s;
    QStringList parts, poles;
    bool ok = false;
    int order = 0;
    auto run = [&](auto& f) {
        ok = f.calcFilter();
        f.createPolesZerosList(poles);
        f.createPartList(parts);
        if (ok) f.createSchematic(s);
    };
    if (t == 0) { SallenKey f(ffunc, ftyp, par); run(f); }
    else if (t == 1) { MFBfilter f(ffunc, ftyp, par); run(f); }
    else { SchCauer f(ffunc, ftyp, par); run(f); }
    // ("Filter order = 5")
    for (const QString& line : std::as_const(poles))
        if (line.contains(QLatin1String("order"), Qt::CaseInsensitive) && line.contains(QLatin1Char('=')))
            order = line.section(QLatin1Char('='), 1).trimmed().toInt();
    if (!ok || s.isEmpty()) {
        QJsonObject e = error(QStringLiteral("this filter cannot be made with these values and realization"));
        e.insert("calculated", QJsonArray::fromStringList(poles + parts));
        return e;
    }
    QJsonObject result{{"schematic", s}, {"parts", QJsonArray::fromStringList(parts)}, {"type", response},
                       {"class", classes.at(c)}, {"realization", topology}};
    if (order > 0) result.insert("order", order);
    return result;
}

} // namespace


// #########################################################################
// Loads the settings file and stores the settings.
bool loadSettings()
{
    QSettings settings("qucs","qucs_s");
    settings.beginGroup("QucsActiveFilter");
    if(settings.contains("x"))QucsSettings.x=settings.value("x").toInt();
    if(settings.contains("y"))QucsSettings.y=settings.value("y").toInt();
    if(settings.contains("showConsole")) QucsSettings.showConsole=settings.value("showConsole").toBool();
    settings.endGroup();

    if(settings.contains("Language"))QucsSettings.Language=settings.value("Language").toString();

  return true;
}


// #########################################################################
// Saves the settings in the settings file.
bool saveApplSettings(QucsActiveFilter *qucs)
{
    QSettings settings ("qucs","qucs_s");
    settings.beginGroup("QucsActiveFilter");
    settings.setValue("x", qucs->x());
    settings.setValue("y", qucs->y());
    settings.setValue("showConsole", QucsSettings.showConsole);
    settings.endGroup();
  return true;

}


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

    QString LangDir;
    // apply default settings
    QucsSettings.x = 200;
    QucsSettings.y = 100;
    QucsSettings.showConsole = true;

    // is application relocated?
    QDir QucsDir;
    QString QucsApplicationPath = QCoreApplication::applicationDirPath();
#ifdef __APPLE__
    QucsDir = QDir(QucsApplicationPath.section("/bin",0,0));
#else
    QucsDir = QDir(QucsApplicationPath);
    QucsDir.cdUp();
#endif
    LangDir = QucsDir.canonicalPath() + "/share/" QUCS_NAME "/lang/";

    loadSettings();

    QTranslator tor( 0 );
    QString Lang = QucsSettings.Language;
    if(Lang.isEmpty())
      Lang = QString(QLocale::system().name());
    static_cast<void>(tor.load( QStringLiteral("qucs_") + Lang, LangDir));
    a.installTranslator( &tor );

    QucsActiveFilter *w = new QucsActiveFilter();
    w->raise();
    w->move(QucsSettings.x, QucsSettings.y);  // position before "show" !!!
    w->show();
    
    int result = a.exec();
    saveApplSettings(w);
    return result;
}
