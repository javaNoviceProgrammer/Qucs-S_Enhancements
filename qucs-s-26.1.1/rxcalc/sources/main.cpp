/*
 * Copyright 2014, 2015 Verkhovin Vyacheslav
 *
 * This file is part of RxCalc.
 *
 * RxCalc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RxCalc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RxCalc. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "main.h"
#include "system.h"
#include "stage.h"
#include "../../qucs/tooljson.h"

#include <QCoreApplication>
#include <QJsonArray>

#include <cmath>

// --json: a receiver's stages on standard input, its cascade - each
// stage's and the system's gain, noise figure, IP3, P1dB, noise floor,
// sensitivity, dynamic range - on standard output, as the window computes
// them. Nothing is shown.
namespace {

namespace tj = qucs_s::tooljson;

QJsonValue num(float v)
{
    return std::isfinite(v) ? QJsonValue(std::round(double(v) * 1e4) / 1e4) : QJsonValue(QJsonValue::Null);
}

QJsonObject cascade(const QJsonObject& spec)
{
    auto error = [](const QString& why) { return QJsonObject{{QStringLiteral("error"), why}}; };
    const QJsonArray stages = spec.value("stages").toArray();
    if (stages.isEmpty()) return error(QStringLiteral("'stages' lists the receiver's stages, first the input's: [{\"name\", \"gain\", \"nf\", \"iip3\" or \"oip3\", \"ip1db\" or \"op1db\"}]"));
    System system;
    if (spec.contains("input_power")) system.setInputPower(tj::number(spec.value("input_power")));
    if (spec.contains("bandwidth")) system.setNoiseBand(tj::number(spec.value("bandwidth")));
    if (spec.contains("snr_min")) system.setMinSignalToNoise(tj::number(spec.value("snr_min")));
    if (spec.contains("temperature")) system.setTemperature_C(tj::number(spec.value("temperature")));
    if (spec.contains("peak_to_average")) system.setPeakToRatio(tj::number(spec.value("peak_to_average")));
    QStringList assumed;
    int n = 0;
    for (const QJsonValue& v : stages) {
        const QJsonObject o = v.toObject();
        auto* stage = new Stage();
        const QString name = o.value("name").toString(QStringLiteral("stage %1").arg(n + 1));
        stage->setName(name);
        stage->setEnabled(o.value("enabled").toBool(true));
        // (The gain first: an IP3 or P1dB is kept as given, the other side from it.)
        stage->setPowerGain(o.contains("gain") ? tj::number(o.value("gain")) : 0.0);
        stage->setNoiseFigure(o.contains("nf") ? tj::number(o.value("nf")) : std::max(0.0, -tj::number(o.value("gain"))));
        if (o.contains("oip3")) stage->setOip3(tj::number(o.value("oip3")));
        else if (o.contains("iip3")) stage->setIip3(tj::number(o.value("iip3")));
        else {
            stage->setIip3(100);
            assumed << QStringLiteral("%1's IIP3 +100 dBm").arg(name);
        }
        if (o.contains("op1db")) stage->setOp1db(tj::number(o.value("op1db")));
        else if (o.contains("ip1db")) stage->setIp1db(tj::number(o.value("ip1db")));
        else {
            stage->setIp1db(100);
            assumed << QStringLiteral("%1's input P1dB +100 dBm").arg(name);
        }
        if (!o.contains("nf") && !o.contains("gain")) assumed << QStringLiteral("%1: no gain, no noise figure").arg(name);
        system.stageList->append(stage);
        ++n;
    }
    system.solve();
    QJsonArray each;
    for (Stage* s : *system.stageList) {
        each.append(QJsonObject{{"name", s->name()},
                                {"enabled", s->enabled()},
                                {"gain to here, dB", num(s->sys.powerGain)},
                                {"noise figure to here, dB", num(s->sys.noiseFigure)},
                                {"iip3 to here, dBm", num(s->sys.iip3)},
                                {"oip3 to here, dBm", num(s->sys.oip3)},
                                {"input p1db to here, dBm", num(s->sys.ip1db)},
                                {"output p1db to here, dBm", num(s->sys.op1db)},
                                {"input power, dBm", num(s->sys.inputPower)},
                                {"output power, dBm", num(s->sys.outputPower)},
                                {"its part of the noise figure in dB (0 to 1)", num(s->sys.noiseFigureToSystemNoiseFigure)},
                                {"its part of the iip3 (0 to 1)", num(s->sys.stageIip3ToSystemIip3)},
                                {"output backoff from p1db, dB", num(s->sys.powerOutBackoff)}});
        delete s;
    }
    system.stageList->clear();
    const auto& y = system.sys1;
    QJsonObject r{{"stages", each},
                  {"system", QJsonObject{{"gain, dB", num(y.sysPowerGain)},
                                         {"noise figure, dB", num(y.sysNoiseFigure)},
                                         {"iip3, dBm", num(y.sysIip3)},
                                         {"oip3, dBm", num(y.sysOip3)},
                                         {"input p1db, dBm", num(y.sysIp1db)},
                                         {"output p1db, dBm", num(y.sysOp1db)},
                                         {"output power, dBm", num(y.sysOutputPower)},
                                         {"noise floor at the input, dBm/Hz", num(y.sysNoiseFloor_dbmHz)},
                                         {"noise at the output in the bandwidth, dBm", num(y.sysNoiseFloor_dbm)},
                                         {"noise density at the output, dBm/Hz", num(y.sysOutputNsd_dbmHz)},
                                         {"snr, dB", num(y.snr)},
                                         {"mds, dBm", num(y.mds)},
                                         {"noise temperature, K", num(y.noiseTemperature)},
                                         {"sensitivity, dBm", num(y.sensivity)},
                                         {"im level at the input, dBm", num(y.inputImLevel_dBm)},
                                         {"im level at the output, dBm", num(y.outputImLevel_dBm)},
                                         {"imd, dBc", num(y.imd)},
                                         {"sfdr, dB", num(y.sfdr)},
                                         {"blocking dynamic range, dB", num(y.bdr)}}},
                  {"conditions", QJsonObject{{"input power, dBm", system.inputPower()},
                                             {"noise bandwidth, Hz", system.noiseBand()},
                                             {"minimum snr, dB", system.minSignalToNoise()},
                                             {"temperature, C", system.temperature_C()}}}};
    if (!assumed.isEmpty()) r.insert("assumed", QJsonArray::fromStringList(assumed));
    return r;
}

} // namespace

int main(int argc, char *argv[])
{
    if (qucs_s::tooljson::wanted(argc, argv)) {
        QCoreApplication app(argc, argv);
        QString error;
        const QJsonObject spec = qucs_s::tooljson::spec(&error);
        if (!error.isEmpty()) return qucs_s::tooljson::fail(error);
        return qucs_s::tooljson::answer(cascade(spec));
    }
    //QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF8"));

    QApplication app(argc, argv);

    RxCalc = new RxCalcApp(argc, argv);
    app.setActiveWindow(RxCalc);
    RxCalc->show();
    
    return app.exec();
}
