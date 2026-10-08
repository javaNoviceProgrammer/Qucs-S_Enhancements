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
#include "../qucs/symbolstyle.h"

#include <QJsonArray>

#include <cmath>
#include <limits>

struct tQucsSettings QucsSettings;

// #########################################################################
// --json: a spec on standard input, the schematic "Calculate and copy to
// clipboard" makes - with its parts and order - on standard output.
// Nothing is shown, and no setting is read or written.
namespace {

namespace tj = qucs_s::tooljson;

// The window's order calculation alone, for a Butterworth or a Chebyshev:
// the order its stop band gives, or -1 when that is more than it makes.
class OrderOf : public Filter
{
public:
    using Filter::Filter;
    int order()
    {
        const bool ok = ffunc == Filter::Butterworth ? calcButterworth() : calcChebyshev();
        return ok && !Poles.isEmpty() ? int(Poles.count()) : -1;
    }
};

// A Butterworth's or a Chebyshev's poles are its order's alone: the stop
// band only gives the order. So an order given is made by a stop band that
// gives it - 'fs' for a low- or high-pass, 'transition' for a band - found
// by halving, as the order falls the further out it is. False when no stop
// band gives it (a band-stop's order is even).
bool stopBandFor(int wanted, Filter::FilterFunc ffunc, Filter::FType ftyp, FilterParam& par)
{
    const auto set = [&](FilterParam& p, double out) {   // out above 1: how far out
        if (ftyp == Filter::LowPass) p.Fs = par.Fc * out;
        else if (ftyp == Filter::HighPass) p.Fs = par.Fc / out;
        else p.TW = par.Fl * (1.0 - 1.0 / out);
    };
    const auto orderAt = [&](double out) {
        FilterParam p = par;
        set(p, out);
        const int n = OrderOf(ffunc, ftyp, p).order();
        return n < 0 ? std::numeric_limits<int>::max() : n;
    };
    double near = std::log(1.0 + 1e-12), far = std::log(1e12);
    if (orderAt(std::exp(far)) > wanted || orderAt(std::exp(near)) < wanted) return false;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (near + far);
        const int n = orderAt(std::exp(mid));
        if (n == wanted) {
            set(par, std::exp(mid));
            return true;
        }
        (n > wanted ? near : far) = mid;
    }
    return false;
}

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
    // Whose order is given: a Bessel's and a Legendre's always, a
    // Butterworth's and a Chebyshev's when 'order' is (else 'atten' at the
    // stop band gives it). An inverse Chebyshev's or a Cauer's zeros are
    // where its stop band is: its order comes from that.
    const bool ordered = ffunc == Filter::Bessel || ffunc == Filter::Legendre;
    const bool byOrder = par.order > 0 && (ffunc == Filter::Butterworth || ffunc == Filter::Chebyshev);
    if (ftyp == Filter::LowPass || ftyp == Filter::HighPass) {
        par.Ap = spec.contains("ap") ? tj::number(spec.value("ap")) : 3.0;
        par.Fc = tj::number(spec.value("fc"));
        par.Fs = tj::number(spec.value("fs"));
        if (!(par.Fc > 0)) return error(QStringLiteral("'fc' is the cutoff frequency in Hz"));
        if (!(par.Fs > 0) && !ordered && !byOrder)
            return error(ffunc == Filter::Butterworth || ffunc == Filter::Chebyshev
                             ? QStringLiteral("give 'order', or 'fs': the stop band's frequency in Hz, where 'atten' is reached, gives it")
                             : QStringLiteral("'fs' is the stop band's frequency in Hz, where 'atten' is reached: it gives the order"));
        // (A stop band frequency the other side of the cutoff keeps the
        // window's checks quiet.)
        if (ordered && !(par.Fs > 0)) par.Fs = ftyp == Filter::LowPass ? 2.0 * par.Fc : par.Fc / 2.0;
    } else {
        par.Fl = tj::number(spec.value("fc"));
        par.Fu = tj::number(spec.value("f2"));
        par.TW = tj::number(spec.value("transition"));
        if (!(par.Fl > 0) || !(par.Fu > par.Fl))
            return error(QStringLiteral("a band-pass or band-stop filter is from 'fc' to 'f2' (Hz), 'f2' above 'fc'"));
        if (!(par.TW > 0) && !ordered && !byOrder)
            return error(QStringLiteral("'transition' is the width in Hz from each band edge to where 'atten' is reached: it gives the order"));
    }
    QString note;
    if (byOrder) {
        if (spec.contains("fs") || spec.contains("transition") || spec.contains("atten") || spec.contains("ap"))
            note = QStringLiteral("the order given is made: the stop band ('fs' or 'transition', 'atten') and 'ap' are not used for it");
        // (They only gave the order: the poles are the order's alone.)
        par.As = 20.0;
        par.Ap = 3.0;
        if (!stopBandFor(par.order, ffunc, ftyp, par))
            return error(ftyp == Filter::BandStop && par.order % 2 != 0
                             ? QStringLiteral("a band-stop filter's order is even")
                             : QStringLiteral("no %1 filter of order %2 is made here").arg(response).arg(par.order));
    } else if (par.order > 0 && !ordered) {
        note = QStringLiteral("an inverse Chebyshev's or a Cauer filter's order comes from 'atten' at its stop band: the order given is not used");
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
    // (Its resistors drawn as Qucs-S draws new parts, US or European.)
    QJsonObject result{{"schematic", qucs_s::symbols::styled(s, qucs_s::symbols::forTools())}, {"parts", QJsonArray::fromStringList(parts)}, {"type", response},
                       {"class", classes.at(c)}, {"realization", topology}};
    if (order > 0) result.insert("order", order);
    if (!note.isEmpty()) result.insert("note", note);
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
