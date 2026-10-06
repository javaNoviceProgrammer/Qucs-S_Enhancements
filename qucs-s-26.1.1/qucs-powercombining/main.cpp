/*
 * main.cpp - Power combining tool main
 *
 * copyright (C) 2016 Andres Martinez-Mera <andresmartinezmera@gmail.com>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 *
 */

#include <QApplication>
#include "qucspowercombiningtool.h"
#include "../qucs/extsimkernels/spicecompat.h"
#include "../qucs/tooljson.h"

#include <QComboBox>
#include <QLineEdit>
#include <QRadioButton>
#include <QCheckBox>

struct tQucsSettings QucsSettings;



// #########################################################################
// Loads the settings file and stores the settings.
bool loadSettings()
{
    QSettings settings("qucs","qucs_s");
    settings.beginGroup("QucsPowercombining");
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
bool saveApplSettings(QucsPowerCombiningTool *qucs)
{
    QSettings settings ("qucs","qucs_s");
    settings.beginGroup("QucsPowercombining");
    settings.setValue("x", qucs->x());
    settings.setValue("y", qucs->y());
    settings.endGroup();
  return true;

}

// #########################################################################
// --json: a spec on standard input; the window, made but not shown, is
// filled in from it and Generate pressed - the schematic it would put into
// the clipboard on standard output. No setting is read or written.
namespace {

namespace tj = qucs_s::tooljson;

QJsonObject design(QucsPowerCombiningTool* w, const QJsonObject& spec)
{
    auto error = [](const QString& why) { return QJsonObject{{QStringLiteral("error"), why}}; };
    static const QStringList types{"wilkinson", "multistage_wilkinson", "tee", "branchline", "double_box_branchline",
                                   "bagley", "gysel", "travelling_wave", "tree"};
    QString type = spec.value("type").toString(QStringLiteral("wilkinson")).toLower().replace(QLatin1Char(' '), QLatin1Char('_')).replace(QLatin1Char('-'), QLatin1Char('_'));
    if (type == QLatin1String("t_junction") || type == QLatin1String("t")) type = QStringLiteral("tee");
    if (type == QLatin1String("traveling_wave")) type = QStringLiteral("travelling_wave");
    const int t = int(types.indexOf(type));
    if (t < 0) return error(QStringLiteral("'type' is one of %1, not %2").arg(types.join(", "), type));
    const double f = tj::number(spec.value("f"));
    if (!(f > 0)) return error(QStringLiteral("'f' is the frequency in Hz"));
    const double z0 = spec.contains("z0") ? tj::number(spec.value("z0")) : 50.0;
    if (!(z0 > 0)) return error(QStringLiteral("'z0' is in ohms, above 0"));
    w->TopoCombo->setCurrentIndex(t);   // (its choices for the others follow)
    w->RefImplineEdit->setText(QString::number(z0));
    // The frequency in the unit that gives it a few digits, as written: the
    // combo's GHz, MHz, kHz, Hz.
    const double scales[] = {1e9, 1e6, 1e3, 1.0};
    int u = 3;
    for (int i = 0; i < 4; ++i)
        if (f >= scales[i]) { u = i; break; }
    w->FreqScaleCombo->setCurrentIndex(u);
    w->FreqlineEdit->setText(QString::number(f / scales[u], 'g', 12));
    if (spec.contains("ratio_db")) {
        if (t != 0 && t != 2 && t != 3) return error(QStringLiteral("'ratio_db' (an unequal split) is for wilkinson, tee and branchline only"));
        w->K1lineEdit->setText(QString::number(tj::number(spec.value("ratio_db"))));
    }
    if (spec.contains("ways")) {
        const QString ways = QString::number(spec.value("ways").toInt());
        if (w->BranchesCombo->isEditable() && w->BranchesCombo->isEnabled()) w->BranchesCombo->setEditText(ways);
        else if (w->BranchesCombo->findText(ways) >= 0) w->BranchesCombo->setCurrentIndex(w->BranchesCombo->findText(ways));
        else {
            QStringList can;
            for (int i = 0; i < w->BranchesCombo->count(); ++i) can << w->BranchesCombo->itemText(i);
            return error(QStringLiteral("a %1 has %2 ways, not %3").arg(type, can.join(QStringLiteral(" or ")), ways));
        }
    }
    if (spec.contains("stages")) {
        if (t != 1) return error(QStringLiteral("'stages' is for multistage_wilkinson"));
        const int k = w->NStagesCombo->findText(QString::number(spec.value("stages").toInt()));
        if (k < 0) return error(QStringLiteral("a multistage Wilkinson has 2 to 7 stages"));
        w->NStagesCombo->setCurrentIndex(k);
    }
    // Under a SPICE simulator the window offers lumped elements only (and
    // only the Wilkinson combiners have them); Qucsator has ideal lines and
    // microstrip too.
    const bool spice = QucsSettings.DefaultSimulator != spicecompat::simQucsator;
    const QString implementation = spec.value("implementation").toString(spice ? QStringLiteral("lumped") : QStringLiteral("ideal")).toLower();
    QRadioButton* chosen = implementation == QLatin1String("microstrip") ? w->MicrostripradioButton
                           : implementation == QLatin1String("lumped")   ? w->LumpedElementsradioButton
                           : implementation == QLatin1String("ideal")    ? w->IdealTLradioButton
                                                                         : nullptr;
    if (chosen == nullptr) return error(QStringLiteral("'implementation' is ideal (transmission lines), microstrip or lumped"));
    if (!chosen->isEnabled())
        return error(spice && implementation != QLatin1String("lumped")
                         ? QStringLiteral("for a SPICE simulator a combiner is made of lumped elements only: implementation lumped "
                                          "(a Wilkinson), or simulator qucsator for ideal lines and microstrip")
                         : spice ? QStringLiteral("for a SPICE simulator only the Wilkinson combiners can be made (of lumped elements): "
                                                  "type wilkinson or multistage_wilkinson, or simulator qucsator for the others")
                                 : QStringLiteral("only the Wilkinson combiners have a lumped (CLC) implementation"));
    chosen->setChecked(true);
    chosen->click();
    if (spec.contains("alpha")) w->AlphalineEdit->setText(QString::number(tj::number(spec.value("alpha"))));
    const QJsonObject sub = spec.value("substrate").toObject();
    auto set = [&sub](const char* key, QLineEdit* e, double scale) {
        if (sub.contains(key)) e->setText(QString::number(tj::number(sub.value(key)) / scale, 'g', 12));
    };
    if (sub.contains("er")) w->RelPermcomboBox->setEditText(QString::number(tj::number(sub.value("er"))));
    set("h", w->SubstrateHeightlineEdit, 1e-3);
    set("t", w->ThicknesslineEdit, 1e-6);
    set("min_width", w->MinWidthlineEdit, 1e-3);
    set("max_width", w->MaxWidthlineEdit, 1e-3);
    set("tand", w->tanDlineEdit, 1.0);
    set("resistivity", w->ResistivitylineEdit, 1.0);
    set("roughness", w->RoughnesslineEdit, 1.0);
    w->AddSparcheckBox->setChecked(spec.value("s_parameters").toBool(true));
    const QString s = w->generate(true);
    if (s.isEmpty()) return error(QStringLiteral("the network could not be made with these values"));
    return QJsonObject{{"schematic", s}, {"type", type}, {"implementation", implementation}};
}

} // namespace

int main(int argc, char *argv[])
{
    if (qucs_s::tooljson::wanted(argc, argv)) {
        // (Its window is filled in, never shown: no display needed.)
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication app(argc, argv);
        QString error;
        const QJsonObject spec = qucs_s::tooljson::spec(&error);
        if (!error.isEmpty()) return qucs_s::tooljson::fail(error);
        QucsSettings.DefaultSimulator = qucs_s::tooljson::simulator(spec);
        QucsPowerCombiningTool w;
        return qucs_s::tooljson::answer(design(&w, spec));
    }
    // apply default settings
    QucsSettings.x = 200;
    QucsSettings.y = 100;

    QApplication app(argc, argv);

    loadSettings();

    QTranslator tor( 0 );
    QString lang = QucsSettings.Language;
    if(lang.isEmpty())
      lang = QString(QLocale::system().name());
    static_cast<void>(tor.load( QStringLiteral("qucs_") + lang, QucsSettings.LangDir));
    app.installTranslator( &tor );

    QucsPowerCombiningTool *PowerCombiningTool = new QucsPowerCombiningTool();
    PowerCombiningTool->raise();
    PowerCombiningTool->resize(350, 350);
    PowerCombiningTool->move(QucsSettings.x, QucsSettings.y);
    PowerCombiningTool->show();
    int result = app.exec();
    saveApplSettings(PowerCombiningTool);
    return result;
}
