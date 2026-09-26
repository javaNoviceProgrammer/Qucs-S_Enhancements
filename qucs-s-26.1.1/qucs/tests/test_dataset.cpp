/*
 * A simulation's results as numbers (dataset.h): a dataset read - real,
 * complex, swept - its variables found by the names people use, its
 * curves measured (values between samples, crossings, rise and fall,
 * overshoot, settling, period, duty cycle, bandwidth, statistics); a
 * simulator's errors read out of what it printed (simulatorlog.h); a
 * net's new name put into the traces that show its voltage.
 */
#include <QtTest>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>

#include "dataset.h"
#include "qucscontrol_p.h"
#include "simulatorlog.h"

namespace ds = qucs_s::dataset;

namespace {

// A dataset in Qucs-S's format: each variable's values, one to a line.
QString datasetText(const QList<std::tuple<QString, QString, QStringList>>& vars)
{
    QString s = QStringLiteral("<Qucs Dataset 26.1.3>\n");
    for (const auto& [head, deps, values] : vars) {
        const bool indep = head.startsWith(QLatin1String("indep"));
        s += QStringLiteral("<%1%2>\n").arg(head, deps.isEmpty() ? QString() : QLatin1Char(' ') + deps);
        for (const QString& v : values) s += QStringLiteral("  %1\n").arg(v);
        s += indep ? QStringLiteral("</indep>\n") : QStringLiteral("</dep>\n");
    }
    return s;
}

ds::Curve curve(int n, double x0, double x1, const std::function<double(double)>& f)
{
    ds::Curve c;
    for (int i = 0; i < n; ++i) {
        const double x = x0 + (x1 - x0) * i / (n - 1);
        c.x << x;
        c.y << f(x);
    }
    return c;
}

bool near(double a, double b, double relative)
{
    return std::abs(a - b) <= relative * std::max(std::abs(a), std::abs(b));
}

} // namespace

class TestDataset : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString write(const QString& name, const QString& text)
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return QString();
        f.write(text.toUtf8());
        return path;
    }

private slots:
    // Real, complex and swept variables, with what they depend on.
    void aDatasetIsRead()
    {
        const QString path = write(QStringLiteral("rc.dat.ngspice"), datasetText({
            {QStringLiteral("indep time 3"), QString(), {"0", "1e-9", "2e-9"}},
            {QStringLiteral("dep tran.v(out)"), QStringLiteral("time"), {"0", "+5.0e-01", "+1.0e+00"}},
            {QStringLiteral("indep frequency 2"), QString(), {"1e3", "1e6"}},
            {QStringLiteral("dep ac.v(out)"), QStringLiteral("frequency"), {"+1.0e+00+j0.0e+00", "+0.0e+00-j5.0e-01"}},
            {QStringLiteral("indep r 2"), QString(), {"1000", "2000"}},
            {QStringLiteral("dep tran.v(sw)"), QStringLiteral("time r"), {"1", "2", "3", "4", "5", "6"}},
        }));
        ds::Dataset data;
        QString error;
        QVERIFY2(data.read(path, &error), qPrintable(error));
        QCOMPARE(data.variables().size(), 6);
        const ds::Variable* out = data.find(QStringLiteral("tran.v(out)"));
        QVERIFY(out != nullptr);
        QVERIFY(!out->independent);
        QCOMPARE(out->dependencies, QStringList{QStringLiteral("time")});
        QCOMPARE(out->re, (QVector<double>{0, 0.5, 1}));
        QVERIFY(!out->isComplex());
        const ds::Variable* ac = data.find(QStringLiteral("ac.v(out)"));
        QVERIFY(ac != nullptr && ac->isComplex());
        QCOMPARE(ac->im, (QVector<double>{0, -0.5}));
        QVERIFY(data.find(QStringLiteral("time"))->independent);

        // A swept variable: a curve for each value of the parameter.
        QList<QList<QPair<QString, double>>> outer;
        const QList<ds::Curve> curves = ds::curvesOf(data, *data.find(QStringLiteral("tran.v(sw)")), &outer);
        QCOMPARE(curves.size(), 2);
        QCOMPARE(curves.at(1).y, (QVector<double>{4, 5, 6}));
        QCOMPARE(curves.at(1).x, (QVector<double>{0, 1e-9, 2e-9}));
        QCOMPARE(outer.at(1).first().first, QStringLiteral("r"));
        QCOMPARE(outer.at(1).first().second, 2000.0);
        // Complex: the curves are of the magnitude, the phase aside.
        const QList<ds::Curve> acCurves = ds::curvesOf(data, *ac);
        QCOMPARE(acCurves.first().y, (QVector<double>{1, 0.5}));
        QCOMPARE(ds::phasesOf(data, *ac).first().at(1), -90.0);

        QVERIFY(!data.read(write(QStringLiteral("not.dat"), QStringLiteral("hello\n")), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!data.read(dir.filePath(QStringLiteral("missing.dat")), &error));
    }

    // Names as people give them: exact, in another case, without the
    // analysis, a node's name; a simulator's prefix left out.
    void namesAreResolved()
    {
        const QString spice = write(QStringLiteral("n.dat.ngspice"), datasetText({
            {QStringLiteral("indep time 1"), QString(), {"0"}},
            {QStringLiteral("dep tran.v(out)"), QStringLiteral("time"), {"1"}},
            {QStringLiteral("dep tran.i(v1)"), QStringLiteral("time"), {"1"}},
            {QStringLiteral("indep frequency 1"), QString(), {"1"}},
            {QStringLiteral("dep ac.v(out)"), QStringLiteral("frequency"), {"1"}},
        }));
        ds::Dataset data;
        QVERIFY(data.read(spice));
        QCOMPARE(data.resolve(QStringLiteral("tran.v(out)")), QStringList{QStringLiteral("tran.v(out)")});
        QCOMPARE(data.resolve(QStringLiteral("ngspice/tran.v(out)")), QStringList{QStringLiteral("tran.v(out)")});
        QCOMPARE(data.resolve(QStringLiteral("TRAN.V(OUT)")), QStringList{QStringLiteral("tran.v(out)")});
        QCOMPARE(data.resolve(QStringLiteral("v(out)")), (QStringList{QStringLiteral("tran.v(out)"), QStringLiteral("ac.v(out)")}));
        QCOMPARE(data.resolve(QStringLiteral("out")), (QStringList{QStringLiteral("tran.v(out)"), QStringLiteral("ac.v(out)")}));
        QCOMPARE(data.resolve(QStringLiteral("i(v1)")), QStringList{QStringLiteral("tran.i(v1)")});
        QVERIFY(data.resolve(QStringLiteral("v(nothing)")).isEmpty());

        const QString qucsator = write(QStringLiteral("q.dat"), datasetText({
            {QStringLiteral("indep time 1"), QString(), {"0"}},
            {QStringLiteral("dep out.Vt"), QStringLiteral("time"), {"1"}},
        }));
        QVERIFY(data.read(qucsator));
        QCOMPARE(data.resolve(QStringLiteral("out")), QStringList{QStringLiteral("out.Vt")});

        QCOMPARE(ds::analysisOf(QStringLiteral("tran.v(out)")), QStringLiteral("tran"));
        QCOMPARE(ds::bareName(QStringLiteral("ac.v(out)")), QStringLiteral("v(out)"));
        QCOMPARE(ds::analysisOf(QStringLiteral("v(x.y)")), QString());
        QString sim;
        QCOMPARE(ds::withoutSimulator(QStringLiteral("xyce/tran.V(OUT)"), &sim), QStringLiteral("tran.V(OUT)"));
        QCOMPARE(sim, QStringLiteral("xyce"));
    }

    // Values between the samples, statistics weighted over x.
    void curvesAreSampledAndSummed()
    {
        const ds::Curve line = curve(11, 0, 10, [](double x) { return 2 * x; });
        QCOMPARE(ds::valueAt(line, 2.5), 5.0);
        QCOMPARE(ds::valueAt(line, 10), 20.0);
        QVERIFY(std::isnan(ds::valueAt(line, 11)));
        const ds::Curve part = ds::within(line, 2, 4);
        QCOMPARE(part.x, (QVector<double>{2, 3, 4}));

        // A sine over whole periods: mean 0, RMS amplitude / sqrt 2 - with
        // the samples unevenly spaced, as a simulator's are.
        ds::Curve sine;
        for (int i = 0; i <= 4000; ++i) {
            const double t = std::pow(i / 4000.0, 1.3) * 2e-3;
            sine.x << t;
            sine.y << 3 * std::sin(2 * M_PI * 1e3 * t);
        }
        const ds::Stats s = ds::statsOf(sine);
        QVERIFY2(std::abs(s.mean) < 1e-3, qPrintable(QString::number(s.mean)));
        QVERIFY2(near(s.rms, 3 / std::sqrt(2.0), 1e-3), qPrintable(QString::number(s.rms)));
        QVERIFY(near(s.max, 3, 1e-4));
        QVERIFY(near(s.xMax, 0.25e-3, 1e-2));
        QCOMPARE(ds::rounded(1.23456789012), 1.234568);
    }

    // What a step response, a pulse train and a low pass are measured as.
    void curvesAreMeasured()
    {
        const ds::MeasureOptions o;
        // A first order step: 10-90% in tau ln 9, 2% after tau ln 50.
        const double tau = 1e-6;
        const ds::Curve rc = curve(20001, 0, 10e-6, [tau](double t) { return 1 - std::exp(-t / tau); });
        const QJsonObject rise = ds::measure(rc, QStringLiteral("rise_time"), o);
        QVERIFY2(near(rise.value("value").toDouble(), tau * std::log(9.0), 1e-3), QJsonDocument(rise).toJson().constData());
        QVERIFY(ds::measure(rc, QStringLiteral("fall_time"), o).contains("error"));
        QVERIFY(near(ds::measure(rc, QStringLiteral("overshoot"), o).value("value").toDouble() + 1, 1, 1e-3));
        const QJsonObject settle = ds::measure(rc, QStringLiteral("settling_time"), o);
        QVERIFY2(near(settle.value("value").toDouble(), tau * std::log(50.0), 2e-3), QJsonDocument(settle).toJson().constData());

        // A second order step with damping 0.3: its overshoot is
        // exp(-pi z / sqrt(1 - z^2)).
        const double z = 0.3, w = 2 * M_PI * 1e6, wd = w * std::sqrt(1 - z * z);
        const ds::Curve ringing = curve(40001, 0, 20e-6, [&](double t) {
            return 1 - std::exp(-z * w * t) * (std::cos(wd * t) + z / std::sqrt(1 - z * z) * std::sin(wd * t));
        });
        const QJsonObject over = ds::measure(ringing, QStringLiteral("overshoot"), o);
        QVERIFY2(near(over.value("value").toDouble(), 100 * std::exp(-M_PI * z / std::sqrt(1 - z * z)), 1e-3),
                 QJsonDocument(over).toJson().constData());

        // A pulse train: 1 us period, high 30% of it.
        const ds::Curve pulses = curve(100001, 0, 10e-6, [](double t) {
            const double phase = std::fmod(t, 1e-6);
            return phase > 0.1e-6 && phase < 0.4e-6 ? 1.0 : 0.0;
        });
        QVERIFY(near(ds::measure(pulses, QStringLiteral("period"), o).value("value").toDouble(), 1e-6, 1e-3));
        QVERIFY(near(ds::measure(pulses, QStringLiteral("frequency"), o).value("value").toDouble(), 1e6, 1e-3));
        QVERIFY2(near(ds::measure(pulses, QStringLiteral("duty_cycle"), o).value("value").toDouble(), 30, 1e-2),
                 QJsonDocument(ds::measure(pulses, QStringLiteral("duty_cycle"), o)).toJson().constData());
        const QJsonObject crossings = ds::measure(pulses, QStringLiteral("crossings"), o);
        QCOMPARE(crossings.value("count").toInt(), 20);
        QCOMPARE(crossings.value("at").toArray().first().toObject().value("direction").toString(), QStringLiteral("rising"));

        // A first order low pass: 3 dB down at its corner.
        ds::Curve lowPass;
        for (int i = 0; i <= 400; ++i) {
            const double f = std::pow(10.0, 2 + i / 100.0);   // 100 Hz to 1 MHz
            lowPass.x << f;
            lowPass.y << 1 / std::sqrt(1 + std::pow(f / 1e4, 2));
        }
        const QJsonObject band = ds::measure(lowPass, QStringLiteral("bandwidth"), o);
        QVERIFY2(near(band.value("value").toDouble(), 1e4, 1e-2), QJsonDocument(band).toJson().constData());

        QVERIFY(ds::measure(rc, QStringLiteral("no_such"), o).contains("error"));
        QVERIFY(ds::measure(ds::Curve{{0}, {1}}, QStringLiteral("rise_time"), o).contains("error"));
    }

    // A Nutmeg equation's db(...) is written as complex numbers with no
    // imaginary part: read as real, it keeps its sign - as a magnitude, a
    // curve falling to -52.8 dB rose to +52.8 dB, and its bandwidth was
    // 27 times what it is. A vector with an imaginary part stays complex.
    void aComplexVectorWithNoImaginaryPartIsReal()
    {
        QStringList freq, y, v;
        for (int i = 0; i <= 400; ++i) {
            const double f = std::pow(10.0, i / 50.0);   // 1 Hz to 100 MHz
            freq << QString::number(f, 'e', 12);
            const double mag = 1 / std::sqrt(1 + std::pow(f / 6.1e6, 2));
            y << QString::number(20 * std::log10(mag), 'e', 12) + QStringLiteral("+j0.000000000000e+00");
            v << QString::number(mag, 'e', 12) + QStringLiteral("-j") + QString::number(1e-3, 'e', 12);
        }
        const QString path = write("db.dat.ngspice", datasetText({{"indep frequency 401", "", freq}, {"dep ac.y", "frequency", y},
                                                                   {"dep ac.v(out)", "frequency", v}}));
        ds::Dataset data;
        QVERIFY(data.read(path));
        const ds::Variable* db = data.find("ac.y");
        QVERIFY(db != nullptr);
        QVERIFY(!db->isComplex());
        QVERIFY(db->writtenComplex);
        QVERIFY(db->re.last() < -20);   // falling, not rising
        QVERIFY(data.find("ac.v(out)")->isComplex());
        QVERIFY(!data.find("ac.v(out)")->writtenComplex);

        const ds::Curve curve = ds::curvesOf(data, *db).first();
        QVERIFY(ds::statsOf(curve).max <= 0.001);
        ds::MeasureOptions o;
        // Not known to be in dB: a curve below 0 is not a magnitude either.
        QVERIFY(ds::measure(curve, QStringLiteral("bandwidth"), o).value("error").toString().contains("below 0"));
        o.decibels = true;
        const QJsonObject band = ds::measure(curve, QStringLiteral("bandwidth"), o);
        QVERIFY2(near(band.value("value").toDouble(), 6.1e6, 2e-2), QJsonDocument(band).toJson().constData());
        QCOMPARE(band.value("level").toDouble(), -3.0);
    }

    // What a variable's numbers are: dB, degrees, V, A, s, Hz - from its
    // name, or the equation that makes it.
    void unitsAreToldFromNamesAndEquations()
    {
        QCOMPARE(ds::unitOf("ac.y", "db(norm(v(out)))"), QStringLiteral("dB"));
        QCOMPARE(ds::unitOf("ac.y", "20*log10(mag(v(out)))"), QStringLiteral("dB"));
        QCOMPARE(ds::unitOf("ngspice/ac.vdb(out)"), QStringLiteral("dB"));
        QCOMPARE(ds::unitOf("gain", "dB(out.v)"), QStringLiteral("dB"));
        QCOMPARE(ds::unitOf("ac.cph(out)"), QString(QChar(0x00B0)));
        QCOMPARE(ds::unitOf("tran.v(out)"), QStringLiteral("V"));
        QCOMPARE(ds::unitOf("tran.i(v1)"), QStringLiteral("A"));
        QCOMPARE(ds::unitOf("out.Vt"), QStringLiteral("V"));
        QCOMPARE(ds::unitOf("time"), QStringLiteral("s"));
        QCOMPARE(ds::unitOf("frequency"), QStringLiteral("Hz"));
        QVERIFY(ds::unitOf("ac.y").isEmpty());
        QVERIFY(ds::unitOf("ac.y", "v(out)/v(in)").isEmpty());   // a ratio: no unit said
    }

    // An op analysis's values - a node's, a device's quantity - are one
    // value each that nothing depends on; asking for v(out) gives the
    // operating point's with the analyses'.
    void anOperatingPointIsTold()
    {
        const QString path = write("op.dat.ngspice", datasetText({{"indep v(out) 1", "", {"2.5"}},
                                                                   {"indep @jt1[id] 1", "", {"1e-2"}},
                                                                   {"indep time 3", "", {"0", "1", "2"}},
                                                                   {"dep tran.v(out)", "time", {"0", "1", "2"}}}));
        ds::Dataset data;
        QVERIFY(data.read(path));
        QVERIFY(ds::isOperatingPointValue(data, *data.find("v(out)")));
        QVERIFY(ds::isOperatingPointValue(data, *data.find("@jt1[id]")));
        QVERIFY(!ds::isOperatingPointValue(data, *data.find("time")));
        QCOMPARE(data.resolve("v(out)"), (QStringList{"v(out)", "tran.v(out)"}));
        QCOMPARE(data.resolve("out"), (QStringList{"v(out)", "tran.v(out)"}));
    }

    // ngspice's errors and warnings as it prints them: the message, the
    // netlist line (by its number, or as printed), the part, the node.
    void aSimulatorsProblemsAreRead()
    {
        const QString output = QStringLiteral(
            "Ngspice started...\n"
            "Warning: can't find the initialization file spinit.\n"
            "warning, can't find model 'qmodel' from line\n"
            "    q1 out in 0 qmodel\n"
            "Error: unknown subckt: x1 out 0 nosuch\n"
            "    in line no. 5 from file spice4qucs.cir\n"
            "    Simulation interrupted due to error!\n"
            "\n"
            "Circuit: * qucs\n"
            "Warning: Model issue on line 6 :\n"
            "  .model dx d(is=1e-14 foo=3) ...\n"
            "unrecognized parameter (foo) - ignored\n"
            "\n"
            "Warning: no DC path from node 'mid' to ground; gmin (1e-12 S) installed to provide one\n"
            "Warning: no DC path from node 'mid' to ground; gmin (1e-12 S) installed to provide one\n"
            "doAnalyses: TRAN:  Timestep too small; time = 1e-09, timestep = 1e-21: trouble with node \"out\"\n"
            "Doing analysis at TEMP = 27.000000 and TNOM = 27.000000\n"
            "No. of Data Rows : 229\n");
        const QStringList netlist = {QStringLiteral("* qucs"), QStringLiteral("V1 in 0 DC 1"), QStringLiteral("R1 in out 1k"),
                                     QStringLiteral("Q1 out in 0 qmodel"), QStringLiteral("X1 out 0 nosuch"),
                                     QStringLiteral(".model dx D(IS=1e-14 foo=3)")};
        const QHash<QString, QString> parts = {{QStringLiteral("q1"), QStringLiteral("Q1")},
                                               {QStringLiteral("x1"), QStringLiteral("SUB1")},
                                               {QStringLiteral("r1"), QStringLiteral("R1")}};
        const QList<qucs_s::simlog::Problem> list = qucs_s::simlog::problems(output, netlist, parts);
        QCOMPARE(list.size(), 6);   // the repeated warning once

        QCOMPARE(list.at(1).severity, qucs_s::simlog::Problem::Warning);
        QCOMPARE(list.at(1).netlistLine, QStringLiteral("q1 out in 0 qmodel"));
        QCOMPARE(list.at(1).component, QStringLiteral("Q1"));

        const qucs_s::simlog::Problem subckt = list.at(2);
        QCOMPARE(subckt.severity, qucs_s::simlog::Problem::Error);
        QCOMPARE(subckt.message, QStringLiteral("unknown subckt: x1 out 0 nosuch"));
        QCOMPARE(subckt.line, 5);
        QCOMPARE(subckt.netlistLine, QStringLiteral("X1 out 0 nosuch"));
        QCOMPARE(subckt.component, QStringLiteral("SUB1"));

        const qucs_s::simlog::Problem model = list.at(3);
        QCOMPARE(model.line, 6);
        QCOMPARE(model.netlistLine, QStringLiteral(".model dx d(is=1e-14 foo=3)"));
        QVERIFY2(model.message.contains(QLatin1String("unrecognized parameter (foo)")), qPrintable(model.message));

        QCOMPARE(list.at(4).node, QStringLiteral("mid"));
        QCOMPARE(list.at(5).severity, qucs_s::simlog::Problem::Error);
        QCOMPARE(list.at(5).node, QStringLiteral("out"));

        const QJsonArray json = qucs_s::simlog::toJson(list);
        QCOMPARE(json.at(2).toObject().value("netlist line number").toInt(), 5);
        QCOMPARE(json.at(2).toObject().value("component").toString(), QStringLiteral("SUB1"));

        // A value ngspice cannot read: the line it names, why, the part.
        const QList<qucs_s::simlog::Problem> value = qucs_s::simlog::problems(
            QStringLiteral("Circuit: * undefined\n\n"
                           "Error on line 3 or its substitute:\n"
                           "  r1 in out {nosuch}\n"
                           "numparam: Undefined parameter [nosuch]\n"
                           "Cannot compute substitute: 'nosuch' is not a .param name\n"
                           "  unknown parameter ({nosuch}) \n"
                           "    Simulation interrupted due to error!\n\n"
                           "Error: incomplete or empty netlist\n"
                           "       or no \".plot\", \".print\", or \".fourier\" lines in batch mode;\n"
                           "no simulations run!\n"),
            netlist, parts);
        QCOMPARE(value.size(), 2);
        QCOMPARE(value.first().line, 3);
        QCOMPARE(value.first().netlistLine, QStringLiteral("r1 in out {nosuch}"));
        QCOMPARE(value.first().message, QStringLiteral("numparam: Undefined parameter [nosuch]"));
        QCOMPARE(value.first().component, QStringLiteral("R1"));
        QVERIFY(value.last().message.startsWith(QLatin1String("incomplete or empty netlist or no")));

        // Older ngspice (42): "Netlist line no. 4:" and why.
        const QList<qucs_s::simlog::Problem> older = qucs_s::simlog::problems(
            QStringLiteral("Ngspice started...\n"
                           "Netlist line no. 3:\n"
                           "Undefined parameter [nosuch]\n"
                           "Netlist line no. 3:\n"
                           "Cannot compute substitute\n"
                           "\n"
                           "ERROR: fatal error in ngspice, exit(1)\n"),
            netlist, parts);
        QCOMPARE(older.size(), 3);
        QCOMPARE(older.first().line, 3);
        QCOMPARE(older.first().message, QStringLiteral("Undefined parameter [nosuch]"));
        QCOMPARE(older.first().netlistLine, QStringLiteral("R1 in out 1k"));
        QCOMPARE(older.first().component, QStringLiteral("R1"));
        QCOMPARE(older.last().message, QStringLiteral("fatal error in ngspice, exit(1)"));

        // A device in a subcircuit: the subcircuit's part.
        const QList<qucs_s::simlog::Problem> inside = qucs_s::simlog::problems(
            QStringLiteral("Error on line 3 or its substitute:\n"
                           "  n.xpd1.npd out1 bias popt xpd1:pdmod \n"
                           "Unable to find definition of model xpd1:pdmod\n"),
            {}, {{QStringLiteral("xpd1"), QStringLiteral("PD1")}});
        QCOMPARE(inside.size(), 1);
        QCOMPARE(inside.first().component, QStringLiteral("PD1"));

        // Qucs-S's own check before the simulator.
        const QList<qucs_s::simlog::Problem> check = qucs_s::simlog::problems(
            QStringLiteral("Only DC simulation found in the schematic. It has no effect! Add TRAN, AC, or Sweep simulation to proceed.\n"));
        QCOMPARE(check.size(), 1);
        QCOMPARE(check.first().severity, qucs_s::simlog::Problem::Error);
        QVERIFY(qucs_s::simlog::problems(QStringLiteral("Circuit: x\nNo. of Data Rows : 3\nngspice-46 done\n")).isEmpty());
    }

    // A net's new name wherever a trace or an equation names its voltage,
    // in the case the simulator wrote it; other names left alone.
    void aNetIsRenamedInTraces()
    {
        using qucs_s::control::renameNetIn;
        QCOMPARE(renameNetIn(QStringLiteral("ngspice/tran.v(out)"), QStringLiteral("out"), QStringLiteral("Out1")),
                 QStringLiteral("ngspice/tran.v(out1)"));
        QCOMPARE(renameNetIn(QStringLiteral("ngspice/ac.vdb(out)"), QStringLiteral("out"), QStringLiteral("out1")),
                 QStringLiteral("ngspice/ac.vdb(out1)"));
        QCOMPARE(renameNetIn(QStringLiteral("v(in,out)"), QStringLiteral("out"), QStringLiteral("o2")), QStringLiteral("v(in,o2)"));
        QCOMPARE(renameNetIn(QStringLiteral("v(out, in)"), QStringLiteral("out"), QStringLiteral("o2")), QStringLiteral("v(o2, in)"));
        QCOMPARE(renameNetIn(QStringLiteral("xyce/tran.V(OUT)"), QStringLiteral("out"), QStringLiteral("o2")), QStringLiteral("xyce/tran.V(O2)"));
        QCOMPARE(renameNetIn(QStringLiteral("out.Vt"), QStringLiteral("out"), QStringLiteral("o2")), QStringLiteral("o2.Vt"));
        QCOMPARE(renameNetIn(QStringLiteral("gain=dB(out.v/in.v)"), QStringLiteral("out"), QStringLiteral("o2")),
                 QStringLiteral("gain=dB(o2.v/in.v)"));
        // Not the net: another node, a current, a part's name.
        QCOMPARE(renameNetIn(QStringLiteral("tran.v(output)"), QStringLiteral("out"), QStringLiteral("o2")), QStringLiteral("tran.v(output)"));
        QCOMPARE(renameNetIn(QStringLiteral("tran.i(vout)"), QStringLiteral("out"), QStringLiteral("o2")), QStringLiteral("tran.i(vout)"));
        QCOMPARE(renameNetIn(QStringLiteral("x.out.v"), QStringLiteral("out"), QStringLiteral("o2")), QStringLiteral("x.out.v"));
    }
};

QTEST_MAIN(TestDataset)
#include "test_dataset.moc"
