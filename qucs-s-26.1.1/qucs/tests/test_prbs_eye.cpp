/*
 * test_prbs_eye.cpp - a serial link's two ends: the V(PRBS) source (its
 * ngspice line, NRZ and PAM4, and the bits an ngspice with the PRBS source
 * sends), the eye analysis (the unit interval told from the crossings,
 * the jitter, the height, PAM4's three eyes, the mask, the folding, what
 * it refuses) and the Eye diagram (laid out from a dataset, saved and
 * loaded with the schematic, drawn, edited in the diagram dialog, and the
 * cursor readout).
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPainter>
#include <QProcess>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <random>

#include "config.h"
#include "dataset.h"
#include "eyeanalysis.h"
#include "ink.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "schematic.h"
#include "statusbar.h"
#include "components/component.h"
#include "diagrams/diagramdialog.h"
#include "diagrams/eyediagram.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

namespace eye = qucs_s::eye;
namespace ds = qucs_s::dataset;

constexpr double UI = 100e-12;

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The bits of a maximal-length shift register (x^7 + x^6 + 1).
QVector<int> bits(int n, int seed = 0x7f)
{
    QVector<int> out;
    int reg = seed & 0x7f;
    for (int k = 0; k < n; ++k) {
        const int b = ((reg >> 6) ^ (reg >> 5)) & 1;
        reg = ((reg << 1) | b) & 0x7f;
        out << b;
    }
    return out;
}

// A waveform of \a levels, a symbol each UI from \a t0: each change a
// straight edge \a edge long, starting \a jitter[k] late; sampled every
// \a dt.
ds::Curve waveform(const QVector<double>& levels, double edge, double dt, double t0 = 0.0, const QVector<double>& jitter = {})
{
    QVector<QPointF> corners{{0.0, levels.value(0)}};
    for (int k = 1; k < levels.size(); ++k) {
        if (levels.at(k) == levels.at(k - 1)) continue;
        const double s = t0 + k * UI + jitter.value(k);
        corners << QPointF(s, levels.at(k - 1)) << QPointF(s + edge, levels.at(k));
    }
    const double end = t0 + levels.size() * UI;
    ds::Curve c;
    int j = 0;
    for (double t = 0.0; t <= end; t += dt) {
        while (j + 1 < corners.size() && corners.at(j + 1).x() <= t) ++j;
        double v = corners.at(j).y();
        if (j + 1 < corners.size()) {
            const QPointF a = corners.at(j), b = corners.at(j + 1);
            if (b.x() > a.x() && t > a.x()) v = a.y() + (t - a.x()) / (b.x() - a.x()) * (b.y() - a.y());
        }
        c.x << t;
        c.y << v;
    }
    return c;
}

QVector<double> nrz(int n)
{
    QVector<double> v;
    for (int b : bits(n)) v << double(b);
    return v;
}

// PAM4: two bits a symbol, Gray coded, on 0, 1/3, 2/3, 1.
QVector<double> pam4(int n)
{
    static const double gray[4] = {0.0, 1.0 / 3.0, 1.0, 2.0 / 3.0};   // 00 01 10 11
    const QVector<int> b = bits(2 * n);
    QVector<double> v;
    for (int k = 0; k < n; ++k) v << gray[2 * b.at(2 * k) + b.at(2 * k + 1)];
    return v;
}

QVector<double> gaussian(int n, double sigma, unsigned seed = 7)
{
    std::mt19937 gen(seed);
    std::normal_distribution<double> d(0.0, sigma);
    QVector<double> out;
    for (int i = 0; i < n; ++i) out << d(gen);
    return out;
}

bool writeDataset(const QString& file, const ds::Curve& c, const QString& name = QStringLiteral("v"))
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QTextStream s(&f);
    s << "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time " << c.x.size() << ">\n";
    for (double t : c.x) s << QString::number(t, 'e', 12) << "\n";
    s << "</indep>\n<dep " << name << " time>\n";
    for (double v : c.y) s << QString::number(v, 'e', 12) << "\n";
    s << "</dep>\n";
    return true;
}

// An Eye diagram line, its fields after the decimals as given.
QString eyeLine(const QString& extra = QString())
{
    return QStringLiteral("<Eye 0 300 400 300 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 -1 0.5 1 315 0 225 1 0 0 0 -1%1 \"\" \"\" \"\">")
        .arg(extra);
}

EyeDiagram* makeDiagram(const QString& line, const QStringList& vars = {QStringLiteral("v")})
{
    QString body;
    for (const QString& v : vars) body += QStringLiteral("<\"%1\" #0050c8 1 3 0 0 0>\n").arg(v);
    body += "</Eye>\n";
    QTextStream stream(&body, QIODevice::ReadOnly);
    auto* d = new EyeDiagram();
    if (!d->load(line, &stream)) {
        delete d;
        return nullptr;
    }
    return d;
}

QImage render(Diagram* d, int width)
{
    QImage img(width, d->y2 + 40, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setFont(QFont("Helvetica", 10));
    d->paintDiagram(&p);
    return img;
}

// Pixels of \a area neither white nor grey (the grid, the frame, black).
int coloured(const QImage& img, const QRect& area)
{
    int n = 0;
    const QRect r = area.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (std::max({c.red(), c.green(), c.blue()}) - std::min({c.red(), c.green(), c.blue()}) > 40) ++n;
        }
    return n;
}

int dark(const QImage& img, const QRect& area)
{
    int n = 0;
    const QRect r = area.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (img.pixelColor(x, y).lightness() < 100) ++n;
    return n;
}

} // namespace

class TestPrbsEye : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    Schematic* doc = nullptr;

    Component* placed(const QString& fileLine)
    {
        QString line = fileLine;
        Component* c = getComponentFromName(line, doc);
        if (!c || !c->load(line)) return nullptr;
        c->setSchematic(doc);
        doc->insertRawComponent(c);
        int i = 1;
        for (Port* p : c->Ports) p->Connection->Name = QStringLiteral("n%1").arg(i++);
        return c;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.font = QApplication::font();
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        // (A simulator there: no box saying there is none.)
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        // A box the main window or a loader raises is closed, not waited on.
        QTimer* closer = new QTimer(this);
        connect(closer, &QTimer::timeout, this, [] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->isModal() && w->isVisible()) w->close();
        });
        closer->start(200);
        app = new QucsApp(false);
        QucsMain = app;
        doc = new Schematic(app, "");
    }

    void cleanupTestCase()
    {
        delete doc;
        QucsMain = nullptr;
        delete app;
    }

    // The source: ngspice's PRBS(v1 v2 tbit td tr tf order [seed]), PAM4
    // with the same arguments; empty fields are ngspice's defaults, no seed
    // the register's all ones; for ngspice only, among the sources.
    void theSourceWritesNgspicesPrbs()
    {
        Component* c = placed("<vPRBS V1 1 200 200 18 -26 0 1 \"0 V\" 1 \"1 V\" 1 \"100 ps\" 1 \"0\" 0 \"10 ps\" 0 \"12 ps\" 0 \"7\" 1 \"\" 0 \"NRZ\" 0>");
        QVERIFY(c);
        QCOMPARE(c->Model, QString("vPRBS"));
        QCOMPARE(c->getSpiceNetlist(spicecompat::SPICEDefault).trimmed(), QString("V1 n1 n2 PRBS(0 1 100P 0 10P 12P 7)"));
        c->getProperty("Seed")->Value = "5";
        c->getProperty("Coding")->Value = "PAM4";
        c->getProperty("Order")->Value = "13";
        c->getProperty("Td")->Value = "";
        c->getProperty("Tr")->Value = " ";
        QCOMPARE(c->getSpiceNetlist(spicecompat::SPICEDefault).trimmed(), QString("V1 n1 n2 PAM4(0 1 100P 0 0 12P 13 5)"));
        QVERIFY(c->getProperty("Coding")->Description.endsWith("[NRZ, PAM4]"));
        QCOMPARE(c->Simulator, int(spicecompat::simNgspice));
        QVERIFY2(c->save().startsWith("<vPRBS V1 "), qPrintable(c->save()));

        bool listed = false;
        for (Module* m : Category::getModules(QObject::tr("sources"))) {
            if (!m->info) continue;
            QString name;
            char* file = nullptr;
            (*m->info)(name, file, false);
            if (name == "V(PRBS)") listed = true;
        }
        QVERIFY(listed);
    }

    // What an ngspice with the PRBS source makes of the line: the levels
    // at the bits' centres, PRBS7 repeating every 127 bits with 64 ones,
    // the delay, PAM4's four levels. (Skipped without one.)
    void ngspiceSendsTheBits()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice on PATH");
        auto run = [&](const QString& source, QVector<double>* centres, double td) {
            Component* c = placed(source);
            if (!c) return false;
            const QString deck = dir.filePath("prbs.cir"), out = dir.filePath("prbs.txt");
            QFile f(deck);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            f.write(QStringLiteral("* prbs\n%1R1 n1 0 1k\nR2 n2 0 1k\n.control\ntran 10p 31.2n\nwrdata %2 v(n1)-v(n2)\n.endc\n.end\n")
                        .arg(c->getSpiceNetlist(spicecompat::SPICEDefault), out)
                        .toUtf8());
            f.close();
            QFile::remove(out);
            QProcess p;
            p.start(ngspice, {"-b", deck});
            if (!p.waitForFinished(60000)) return false;
            QFile o(out);
            if (!o.open(QIODevice::ReadOnly)) return false;
            ds::Curve curve;
            for (const QByteArray& line : o.readAll().split('\n')) {
                const QList<QByteArray> w = line.simplified().split(' ');
                if (w.size() < 2) continue;
                curve.x << w.at(0).toDouble();
                curve.y << w.at(1).toDouble();
            }
            for (int k = 0; k < 300; ++k) centres->append(ds::valueAt(curve, td + (k + 0.5) * UI));
            centres->prepend(ds::valueAt(curve, td / 2));   // before the first bit
            return true;
        };
        QVector<double> v;
        if (!run("<vPRBS V1 1 200 200 18 -26 0 1 \"-1 V\" 1 \"1 V\" 1 \"100 ps\" 1 \"1 ns\" 0 \"0\" 0 \"0\" 0 \"7\" 1 \"\" 0 \"NRZ\" 0>", &v, 1e-9)
            || v.size() < 301 || std::isnan(v.at(1)))
            QSKIP("this ngspice has no PRBS source");
        QCOMPARE(v.takeFirst(), -1.0);   // U1 until the delay
        int ones = 0;
        for (int k = 0; k < 300; ++k) {
            QVERIFY2(std::abs(std::abs(v.at(k)) - 1.0) < 1e-6, qPrintable(QString::number(v.at(k))));
            if (k >= 127) QCOMPARE(v.at(k), v.at(k - 127));
            if (k < 127 && v.at(k) > 0) ++ones;
        }
        QCOMPARE(ones, 64);
        for (int k = 0; k < 6; ++k) QCOMPARE(v.at(k), -1.0);   // all ones first: six zeros
        QCOMPARE(v.at(6), 1.0);

        QVector<double> p;
        QVERIFY(run("<vPRBS V1 1 200 200 18 -26 0 1 \"0 V\" 1 \"3 V\" 1 \"100 ps\" 1 \"0\" 0 \"0\" 0 \"0\" 0 \"9\" 1 \"\" 0 \"PAM4\" 0>", &p, 0.0));
        p.removeFirst();
        QSet<int> levels;
        for (double x : p) {
            QVERIFY2(std::abs(x - std::round(x)) < 1e-6, qPrintable(QString::number(x)));
            levels << int(std::round(x));
        }
        QCOMPARE(levels, QSet<int>({0, 1, 2, 3}));
    }

    // Crossings leave a band about the level: ringing within it is none.
    void ringingIsNoCrossing()
    {
        ds::Curve c;
        for (int i = 0; i <= 400; ++i) {
            const double t = i * 1e-12;
            c.x << t;
            // Up at 100 ps through 0.5, ringing +-0.03 about it, up at 300 ps.
            c.y << (t < 100e-12 ? 0.0 : t < 300e-12 ? 0.5 + 0.03 * std::sin(t * 1e11) : 1.0);
        }
        QCOMPARE(eye::crossingTimes(c, 0.5, 0.05).size(), 1);
        QVERIFY(eye::crossingTimes(c, 0.5, 0.0).size() > 3);
    }

    // The unit interval told from the crossings: as given; the jitter, the
    // height and width it measures as with the UI given - the error of a
    // mean of the gaps, folded a UI at a time, would add jitter.
    void theUnitIntervalIsToldFromTheCrossings()
    {
        const QVector<double> jitter = gaussian(400, 2e-12);
        const ds::Curve c = waveform(nrz(400), 10e-12, 2.5e-12, 37e-12, jitter);
        const QVector<double> crossed = eye::crossingTimes(c, 0.5, 0.05);
        QVERIFY(crossed.size() > 150);
        const double estimate = eye::estimateUi(crossed);
        QVERIFY2(std::abs(estimate / UI - 1.0) < 5e-5, qPrintable(QString::number(estimate, 'g', 10)));

        eye::Options o;
        const eye::Result told = eye::analyse(c, o);
        QVERIFY2(told.ok(), qPrintable(told.error));
        QVERIFY(told.uiEstimated);
        o.ui = UI;
        const eye::Result given = eye::analyse(c, o);
        QVERIFY(given.ok() && !given.uiEstimated);
        const eye::Eye& e = given.eyes.first();
        QCOMPARE(given.levels.size(), 2);
        QVERIFY(std::abs(given.levels.at(0)) < 1e-9 && std::abs(given.levels.at(1) - 1.0) < 1e-9);
        QVERIFY(std::abs(e.threshold - 0.5) < 1e-9);
        QVERIFY2(std::abs(e.height - 1.0) < 1e-9, qPrintable(QString::number(e.height)));   // flat at the centres
        // The jitter put in: its rms, and the spread of the edges.
        double lo = 0, hi = 0;
        for (int k = 1; k < 400; ++k)
            if (nrz(400).at(k) != nrz(400).at(k - 1)) {
                lo = std::min(lo, jitter.at(k));
                hi = std::max(hi, jitter.at(k));
            }
        QVERIFY2(std::abs(e.jitterRms / 2e-12 - 1.0) < 0.15, qPrintable(QString::number(e.jitterRms)));
        QVERIFY2(std::abs(e.jitterPp - (hi - lo)) < 0.3e-12, qPrintable(QString::number(e.jitterPp) + " " + QString::number(hi - lo)));
        QVERIFY(std::abs(e.width - (UI - e.jitterPp)) < 1e-15);
        QVERIFY(std::abs(e.widthBer12 - std::max(0.0, UI - 14.069 * e.jitterRms)) < 1e-15);
        // Told, as given: not a picosecond more of jitter.
        QVERIFY2(told.eyes.first().jitterPp < e.jitterPp + 1e-12,
                 qPrintable(QString::number(told.eyes.first().jitterPp) + " " + QString::number(e.jitterPp)));
        QVERIFY(std::abs(told.eyes.first().height - 1.0) < 1e-6);
        // The centre: half a UI from the crossings (at boundary + 5 ps).
        double phase = (given.centre - 37e-12) / UI;
        phase -= std::floor(phase);
        QVERIFY2(std::abs(phase - 0.55) < 0.02, qPrintable(QString::number(phase)));
        // Overshoot (to 1.6 V after each rise): the threshold halfway between
        // the levels, not between the extremes.
        ds::Curve over = waveform(nrz(400), 10e-12, 2.5e-12, 37e-12);
        for (int i = 1; i < over.x.size(); ++i)
            if (over.y.at(i) == 1.0 && over.y.at(i - 1) < 1.0) over.y[i] = 1.6;
        const eye::Result shot = eye::analyse(over, eye::Options());
        QVERIFY2(shot.ok() && std::abs(shot.eyes.first().threshold - 0.5) < 0.01,
                 qPrintable(QString::number(shot.eyes.first().threshold) + shot.error));
        // A threshold given: its crossings.
        o.threshold = 0.25;
        const eye::Result low = eye::analyse(c, o);
        QVERIFY(low.ok());
        QCOMPARE(low.eyes.first().threshold, 0.25);
    }

    // PAM4: four levels at the centres, three eyes; crossings spread over
    // much of a UI - edges of different sizes - still tell the UI.
    void pam4HasThreeEyes()
    {
        const ds::Curve c = waveform(pam4(400), 80e-12, 2.5e-12);
        const QVector<double> crossed = eye::crossingTimes(c, 0.5, 0.05);
        const double estimate = eye::estimateUi(crossed);
        QVERIFY2(std::abs(estimate / UI - 1.0) < 3e-4, qPrintable(QString::number(estimate, 'g', 10)));
        eye::Options o;
        o.levels = 4;
        const eye::Result r = eye::analyse(c, o);
        QVERIFY2(r.ok(), qPrintable(r.error));
        // Fitted again to the edges symmetric about the middle: exact.
        QVERIFY2(r.uiEstimated && std::abs(r.ui / UI - 1.0) < 1e-9, qPrintable(QString::number(r.ui, 'g', 12)));
        QCOMPARE(r.levels.size(), 4);
        for (int i = 0; i < 4; ++i) QVERIFY2(std::abs(r.levels.at(i) - i / 3.0) < 1e-6, qPrintable(QString::number(r.levels.at(i))));
        QCOMPARE(r.eyes.size(), 3);
        for (const eye::Eye& e : r.eyes) {
            QVERIFY2(std::abs(e.height - 1.0 / 3.0) < 1e-6, qPrintable(QString::number(e.height)));
            QVERIFY(e.width > 0 && e.width < UI);
            QVERIFY(e.crossings > 50);
        }
        QVERIFY(std::abs(r.eyes.at(1).threshold - 0.5) < 1e-6);
        QVERIFY(r.notes.isEmpty());

        // NRZ taken for PAM4: no four levels - said.
        const eye::Result two = eye::analyse(waveform(nrz(400), 10e-12, 2.5e-12), o);
        QVERIFY2(!two.ok() && two.error.contains("is it PAM4?"), qPrintable(two.error));
        // Four levels far from evenly spaced (two near each rail, NRZ through
        // a slow channel): an eye, and a doubt said.
        QVector<double> uneven = pam4(400);
        for (double& v : uneven) v = v < 0.2 ? v * 0.3 : v < 0.5 ? 0.1 : v < 0.8 ? 0.9 : 1.0;
        const eye::Result doubt = eye::analyse(waveform(uneven, 10e-12, 2.5e-12), o);
        QVERIFY2(doubt.ok() && doubt.notes.join(" ").contains("far from evenly spaced"),
                 qPrintable(doubt.error + doubt.notes.join(" ")));
    }

    // The mask: a hexagon at the centre; the UIs whose trace enters it.
    void theMaskCountsTheUIsThroughIt()
    {
        QVector<double> levels = nrz(200);
        ds::Curve c = waveform(levels, 10e-12, 2.5e-12);
        eye::Options o;
        o.ui = UI;
        o.maskWidth = 0.5;
        o.maskHeight = 0.5;
        eye::Result r = eye::analyse(c, o);
        QVERIFY(r.ok());
        QCOMPARE(r.maskHits, 0);
        // Taller than the eye: every UI.
        o.maskHeight = 1.2;
        r = eye::analyse(c, o);
        QCOMPARE(r.maskHits, r.symbols);
        // From just after a sampling instant: the UI before it, not sampled,
        // is not counted.
        o.start = 50 * UI + r.centre - std::floor(r.centre / UI) * UI + 5e-12;
        const eye::Result late = eye::analyse(c, o);
        QVERIFY(late.ok());
        QCOMPARE(late.maskHits, late.symbols);
        o.start = NAN;
        // A glitch at one centre: that UI.
        for (int i = 0; i < c.x.size(); ++i)
            if (std::abs(c.x.at(i) - (100 * UI + r.centre - std::floor(r.centre / UI) * UI)) < 3e-12) c.y[i] = 0.55;
        o.maskHeight = 0.5;
        r = eye::analyse(c, o);
        QCOMPARE(r.maskHits, 1);
        // A trace through it between two samples, neither of them in it: a
        // triangle, its corners at the centres.
        ds::Curve triangle;
        for (int k = 0; k <= 60; ++k) {
            triangle.x << k * UI;
            triangle.y << double(k % 2);
        }
        eye::Options t;
        t.ui = UI;
        t.threshold = 0.5;
        // (Its top from -0.2 to 0.2 UI at 0.45 V: the triangle, at 0.5 V at
        // the centre, is in it from 0.05 to 0.2 UI either side.)
        t.maskWidth = 0.8;
        t.maskHeight = 0.9;
        const eye::Result through = eye::analyse(triangle, t);
        QVERIFY2(through.ok(), qPrintable(through.error));
        QVERIFY2(through.maskHits >= through.symbols - 1 && through.maskHits > 50, qPrintable(QString::number(through.maskHits)));
        // Wider than a UI: not tested, said.
        o.maskWidth = 1.5;
        r = eye::analyse(c, o);
        QCOMPARE(r.maskHits, -1);
        QVERIFY(r.notes.join(" ").contains("wider than a UI"));
        QCOMPARE(eye::mask(0.4, 0.2).boundingRect(), QRectF(-0.2, -0.1, 0.4, 0.2));
    }

    // Folding: windows a UI apart, each so many UI long, every sample in as
    // many of them, their ends where the curve crosses them.
    void windowsAreAUIApart()
    {
        ds::Curve c;
        for (int i = 0; i <= 100; ++i) {
            c.x << i * 10e-12;
            c.y << i;
        }
        QMap<double, int> seen;   // each time, how often
        int windows = 0, whole = 0;
        const int n = eye::fold(c, 0.0, UI, 0.0, 2, [&](const QVector<QPointF>& line) {
            ++windows;
            if (line.first().x() == 0.0 && std::abs(line.last().x() - 2 * UI) < 1e-21) ++whole;
            QVERIFY(line.first().x() >= -1e-18 && line.last().x() <= 2 * UI + 1e-18);
            for (int i = 1; i < line.size(); ++i) QVERIFY(line.at(i).x() > line.at(i - 1).x());
            for (const QPointF& p : line) {
                // The window's start from its first point: t = y * 10 ps.
                const double t = p.y() * 10e-12;
                seen[std::round(t / 1e-12)]++;
            }
        });
        QCOMPARE(n, windows);
        QCOMPARE(n, 11);   // from -100 ps to 1000 ps
        QCOMPARE(whole, 9);   // those from 0 to 800 ps, their ends where they cross the curve
        QCOMPARE(seen.value(550), 2);
        QCOMPARE(seen.value(20), 2);
        QCOMPARE(eye::fold(c, 0.0, -1.0, 0.0, 2, [](const QVector<QPointF>&) {}), 0);
    }

    // What has no eye is said, not drawn.
    void whatHasNoEyeIsRefused()
    {
        eye::Options o;
        ds::Curve flat;
        flat.x = {0, 1e-9, 2e-9};
        flat.y = {1, 1, 1};
        QVERIFY(eye::analyse(flat, o).error.contains("flat"));
        ds::Curve back = waveform(nrz(50), 10e-12, 5e-12);
        std::swap(back.x[3], back.x[4]);
        QVERIFY(eye::analyse(back, o).error.contains("does not rise"));
        const ds::Curve c = waveform(nrz(50), 10e-12, 5e-12);
        o.ui = 1e-300;
        QVERIFY(eye::analyse(c, o).error.contains("bit period is too short"));
        o.ui = 2e-9;
        QVERIFY(eye::analyse(c, o).error.contains("fewer than 3 bits"));
        o.ui = UI;
        o.levels = 3;
        QVERIFY(eye::analyse(c, o).error.contains("2 levels"));
        o.levels = 2;
        o.start = 10.0;
        QVERIFY(eye::analyse(c, o).error.contains("nothing after"));
        o.start = NAN;
        o.threshold = 5.0;
        QVERIFY(eye::analyse(c, o).error.contains("does not cross"));
        // Crossings at no regular interval: the UI is not guessed.
        ds::Curve random;
        std::mt19937 gen(3);
        std::uniform_real_distribution<double> gap(30e-12, 170e-12);
        double t = 0;
        for (int k = 0; k < 200; ++k) {
            random.x << t << t + 1e-12;
            random.y << double(k % 2) << double((k + 1) % 2);
            t += gap(gen);
        }
        const eye::Result r = eye::analyse(random, eye::Options());
        QVERIFY2(r.error.contains("cannot be told") && r.error.contains("give it"), qPrintable(r.error));
    }

    // Any curve and any options - nan, inf, huge, tiny, unsorted, repeated
    // times, nonsense settings - are taken without harm (and under ASan
    // and UBSan without a report), quickly.
    void anyCurveIsTakenUnharmed()
    {
        std::mt19937 gen(29);
        auto pick = [&](std::initializer_list<double> values) {
            std::uniform_int_distribution<int> i(0, int(values.size()) - 1);
            return *(values.begin() + i(gen));
        };
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        QElapsedTimer clock;
        clock.start();
        for (int round = 0; round < 300; ++round) {
            ds::Curve c;
            const int n = int(pick({0, 1, 2, 5, 50, 400, 3000}));
            const double dt = pick({1e-12, 2.5e-12, 1e-300, 1e300, 0.0, -1e-12});
            double t = pick({0.0, -1e-9, 1e300, -1e300});
            for (int i = 0; i < n; ++i) {
                c.x << t;
                c.y << (unit(gen) < 0.02 ? pick({NAN, INFINITY, -INFINITY, 1e308, -1e308}) : std::round(unit(gen) * 3) / 3);
                t += unit(gen) < 0.05 ? 0.0 : dt;
            }
            eye::Options o;
            o.ui = pick({NAN, 1e-10, 3.3e-11, 1e-300, -1.0, 0.0, 1e300, INFINITY});
            o.start = pick({NAN, 0.0, 1e-9, -1e300, 1e300});
            o.levels = int(pick({2, 4, 3, -1}));
            o.threshold = pick({NAN, 0.5, 1e300, -INFINITY});
            o.maskWidth = pick({NAN, 0.3, 1.0, 2.0, -1.0, 1e-300});
            o.maskHeight = pick({NAN, 0.2, 1e300, 0.0});
            const eye::Result r = eye::analyse(c, o);
            QVERIFY(r.ok() || !r.error.isEmpty());
            if (r.ok()) {
                QVERIFY(std::isfinite(r.ui) && r.ui > 0);
                eye::fold(c, r.start, r.ui, eye::originFor(r, 2), 2, [](const QVector<QPointF>&) {});
            }
            eye::fold(c, o.start, o.ui, pick({0.0, -1e30, 1e30, NAN}), int(pick({1, 2, 8, 0})), [](const QVector<QPointF>&) {});
            eye::estimateUi(eye::crossingTimes(c, 0.5, pick({0.0, 0.05, -1.0, NAN})));
        }
        QVERIFY2(clock.elapsed() < 20000, qPrintable(QString::number(clock.elapsed())));
        // A diagram of a dataset with nan and inf in it, drawn at any zoom.
        ds::Curve c = waveform(nrz(60), 10e-12, 2.5e-12);
        c.y[10] = NAN;
        c.y[20] = INFINITY;
        const QString file = dir.filePath("junk.dat");
        QVERIFY(writeDataset(file, c));
        QScopedPointer<EyeDiagram> d(makeDiagram(eyeLine(" 1e-10 2 - 2 - 0 1 0.3 0.2")));
        d->loadGraphData(file);
        for (double zoom : {1e-9, 0.01, 1.0, 50.0, 1e12}) {
            QImage img(100, 100, QImage::Format_RGB32);
            QPainter p(&img);
            p.scale(zoom, zoom);
            d->paintDiagram(&p);
        }
    }

    // get_dataset's eye: the same analysis - the UI told or given, PAM4.
    void theDatasetMeasuresTheEye()
    {
        const ds::Curve c = waveform(pam4(300), 80e-12, 2.5e-12);
        ds::MeasureOptions o;
        o.levels = 4;
        const QJsonObject r = ds::measure(c, QStringLiteral("eye"), o);
        QVERIFY2(!r.contains("error"), qPrintable(QJsonDocument(r).toJson()));
        QVERIFY(std::abs(r.value("unit interval").toDouble() / UI - 1.0) < 1e-4);
        QVERIFY(r.contains("unit interval from"));
        QCOMPARE(r.value("eyes").toArray().size(), 3);
        QCOMPARE(r.value("levels").toArray().size(), 4);
        QVERIFY(std::abs(r.value("height").toDouble() - 1.0 / 3.0) < 1e-5);
        o.levels = 2;
        o.period = UI;
        const QJsonObject n = ds::measure(waveform(nrz(300), 10e-12, 2.5e-12), QStringLiteral("eye"), o);
        QVERIFY2(!n.contains("error") && !n.contains("unit interval from"), qPrintable(QJsonDocument(n).toJson()));
        QVERIFY(std::abs(n.value("height").toDouble() - 1.0) < 1e-6);
        QVERIFY(n.value("levels").toObject().contains("high"));
        o.period = -1;
        QVERIFY(ds::measure(c, QStringLiteral("eye"), o).contains("error"));
    }

    // The diagram: its graphs folded at the UI the first one tells, the
    // centre in the middle, the x axis a few UIs long, the y axis from the
    // start on, what was measured written beside it.
    void theDiagramFoldsItsGraphs()
    {
        ds::Curve c = waveform(nrz(200), 10e-12, 2.5e-12);
        for (int i = 0; i < c.x.size() && c.x.at(i) < 1e-9; ++i) c.y[i] = 5.0;   // a start to leave out
        const QString file = dir.filePath("eye.dat");
        QVERIFY(writeDataset(file, c));
        QScopedPointer<EyeDiagram> d(makeDiagram(eyeLine(" - 2 1e-9 2 - 0 1 - -")));
        QVERIFY(d);
        QCOMPARE(d->start, 1e-9);
        d->loadGraphData(file);
        QCOMPARE(d->results().size(), 1);
        const eye::Result& r = d->results().first();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QVERIFY(r.uiEstimated);
        QVERIFY(std::abs(d->foldedUi() / UI - 1.0) < 1e-4);
        QCOMPARE(d->xAxis.low, 0.0);
        QVERIFY(std::abs(d->xAxis.up - 2 * d->foldedUi()) < 1e-18);
        QVERIFY2(d->yAxis.max < 2.0, qPrintable(QString::number(d->yAxis.max)));   // the 5 V before the start left out
        const QStringList lines = d->measurementLines();
        // ("v" has no unit, as tran.v(rx) has V.)
        QVERIFY2(lines.first() == "v" && lines.contains("UI 100 ps, from the crossings") && lines.contains("height 1")
                     && lines.contains("width 100 ps (100.0 % UI)") && lines.contains("jitter 0 s rms, 0 s p-p")
                     && lines.contains("levels 0, 1"),
                 qPrintable(lines.join("\n")));
        QCOMPARE(EyeDiagram::engineering(92.514e-12, "s"), QString("92.51 ps"));
        QCOMPARE(EyeDiagram::engineering(0.99996, "V"), QString("1 V"));
        QCOMPARE(EyeDiagram::engineering(999.96e-3, "V"), QString("1 V"));
        QCOMPARE(EyeDiagram::engineering(0.5, "V"), QString("500 mV"));
        // Three UIs across.
        d->span = 3;
        d->updateGraphData();
        QVERIFY(std::abs(d->xAxis.up - 3 * d->foldedUi()) < 1e-18);
        // A second graph folds at the first one's UI.
        QScopedPointer<EyeDiagram> two(makeDiagram(eyeLine(" - 2 1e-9 2 - 0 1 - -"), {"v", "v"}));
        two->loadGraphData(file);
        QCOMPARE(two->results().size(), 2);
        QVERIFY(two->results().at(0).ok() && two->results().at(1).ok());
        QVERIFY(two->results().at(0).uiEstimated);
        QVERIFY(!two->results().at(1).uiEstimated);
        QCOMPARE(two->results().at(1).ui, two->results().at(0).ui);
        // A sweep's curves: all drawn, the first measured - said.
        {
            const ds::Curve a = waveform(nrz(100), 10e-12, 2.5e-12), b = waveform(nrz(100), 20e-12, 2.5e-12);
            QFile f(dir.filePath("sweep.dat"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QTextStream s(&f);
            s << "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time " << a.x.size() << ">\n";
            for (double t : a.x) s << QString::number(t, 'e', 12) << "\n";
            s << "</indep>\n<indep r 2>\n1\n2\n</indep>\n<dep v time r>\n";
            for (const ds::Curve* c : {&a, &b})
                for (double v : c->y) s << QString::number(v, 'e', 12) << "\n";
            s << "</dep>\n";
        }
        QScopedPointer<EyeDiagram> sweep(makeDiagram(eyeLine(" 1e-10 2 - 2 - 0 1 - -")));
        sweep->loadGraphData(dir.filePath("sweep.dat"));
        QVERIFY2(sweep->measurementLines().contains("measured: the first of its 2 curves"), qPrintable(sweep->measurementLines().join("\n")));
        QVERIFY(sweep->results().first().ok());
        // No data: said.
        QScopedPointer<EyeDiagram> none(makeDiagram(eyeLine(), {"nothing"}));
        none->loadGraphData(file);
        QVERIFY(none->measurementLines().join(" ").contains("no data"));
        QVERIFY(std::isnan(none->foldedUi()));
    }

    // Saved after the common fields, read back; a line without them loads
    // with the defaults, nonsense in them too.
    void theSettingsAreSavedAndLoaded()
    {
        QScopedPointer<EyeDiagram> d(makeDiagram(eyeLine(" 1e-10 3 2e-09 4 0.4 1 0 0.3 0.2")));
        QVERIFY(d);
        QCOMPARE(d->ui, 1e-10);
        QCOMPARE(d->span, 3);
        QCOMPARE(d->start, 2e-9);
        QCOMPARE(d->levels, 4);
        QCOMPARE(d->threshold, 0.4);
        QCOMPARE(d->drawn, int(EyeDiagram::Traces));
        QVERIFY(!d->measurements);
        QCOMPARE(d->maskWidth, 0.3);
        QCOMPARE(d->maskHeight, 0.2);
        const QString saved = d->save();
        QVERIFY2(saved.startsWith("<Eye ") && saved.contains(" -1 1e-10 3 2e-09 4 0.4 1 0 0.3 0.2 \"\" \"\" \"\">"), qPrintable(saved));
        QScopedPointer<EyeDiagram> again(makeDiagram(saved.section('\n', 0, 0)));
        QCOMPARE(again->save(), saved);

        QScopedPointer<EyeDiagram> plain(makeDiagram(eyeLine()));
        QVERIFY(std::isnan(plain->ui) && std::isnan(plain->start) && std::isnan(plain->maskWidth));
        QCOMPARE(plain->span, 2);
        QCOMPARE(plain->levels, 2);
        QCOMPARE(plain->drawn, int(EyeDiagram::Density));
        QVERIFY(plain->measurements);
        QScopedPointer<EyeDiagram> junk(makeDiagram(eyeLine(" -5 99 x 3 y 7 z 2 0.1")));
        QVERIFY(std::isnan(junk->ui));
        QCOMPARE(junk->span, 2);
        QCOMPARE(junk->levels, 2);
        QCOMPARE(junk->drawn, int(EyeDiagram::Density));
        QVERIFY(std::isnan(junk->maskWidth) && std::isnan(junk->maskHeight));   // a mask wider than a UI: none
    }

    // In a schematic: loaded by its tag and saved as it was.
    void aSchematicKeepsIt()
    {
        const QString file = dir.filePath("eye.sch");
        const QString line = eyeLine(" 5e-11 2 - 2 0.5 1 1 0.4 0.3");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n"
                               "</Symbol>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n  %1\n"
                               "\t<\"v\" #0050c8 1 3 0 0 0>\n  </Eye>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
                    .arg(line)
                    .toUtf8());
        f.close();
        Schematic sch(nullptr, file);
        QVERIFY(sch.loadDocument());
        QCOMPARE(sch.a_DocDiags.size(), size_t(1));
        auto* d = dynamic_cast<EyeDiagram*>(sch.a_DocDiags.front());
        QVERIFY(d != nullptr);
        QCOMPARE(d->ui, 5e-11);
        QCOMPARE(d->maskHeight, 0.3);
        QVERIFY(sch.save() >= 0);
        bool listed = false;
        for (Module* m : Category::getModules(QObject::tr("diagrams"))) {
            if (!m->info) continue;
            QString name;
            char* bitmap = nullptr;
            Element* e = (*m->info)(name, bitmap, true);
            if (name == "Eye Diagram") listed = dynamic_cast<EyeDiagram*>(e) != nullptr && QFile::exists(misc::getIconPath(bitmap));
            delete e;
        }
        QVERIFY(listed);
        QFile back(file);
        QVERIFY(back.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(back.readAll()).contains(line.mid(0, line.size() - 1)), "the line survives");
    }

    // Drawn: the density in colour, or the traces in the graph's; the
    // measurements beside the frame, and in its bounds; marked in it.
    void itIsDrawn()
    {
        QVector<double> j = gaussian(200, 3e-12, 11);
        ds::Curve c = waveform(nrz(200), 20e-12, 2.5e-12, 0.0, j);
        const QString file = dir.filePath("draw.dat");
        QVERIFY(writeDataset(file, c));
        QScopedPointer<EyeDiagram> d(makeDiagram(eyeLine(" - 2 - 2 - 0 1 0.3 0.2")));
        QVERIFY(d);
        d->loadGraphData(file);
        QVERIFY(d->results().first().ok());
        const QRect plot(1, 1, d->x2 - 2, d->y2 - 2);
        const QImage density = render(d.data(), d->x2 + 420);
        QVERIFY2(coloured(density, plot) > 3000, qPrintable(QString::number(coloured(density, plot))));
        // The box beside it: text right of the frame; in the bounds.
        const QRect beside(d->x2 + 20, 0, 380, 140);
        QVERIFY2(dark(density, beside) > 200, qPrintable(QString::number(dark(density, beside))));
        const QFontMetricsF fm(QFont("Helvetica", 10));
        QVERIFY(d->paintedRect(fm).right() > d->cx + d->x2 + 150);

        d->drawn = EyeDiagram::Traces;
        d->updateGraphData();
        const QImage traces = render(d.data(), d->x2 + 420);
        int blue = 0;
        for (int y = plot.top(); y <= plot.bottom(); ++y)
            for (int x = plot.left(); x <= plot.right(); ++x) {
                const QColor p = traces.pixelColor(x, y);
                if (p.blue() - p.red() > 40 && p.blue() - p.green() > 20) ++blue;
            }
        QVERIFY2(blue > 2000, qPrintable(QString::number(blue)));

        // Without the measurements: nothing beside it, bounds the frame's.
        d->measurements = false;
        d->updateGraphData();
        const QImage plain = render(d.data(), d->x2 + 420);
        QCOMPARE(dark(plain, beside), 0);
        QVERIFY(d->paintedRect(fm).right() < d->cx + d->x2 + 40);
        // The marks (threshold, centre, arrows, mask) are drawn in the plot.
        QVERIFY(dark(traces, plot) > dark(plain, plot) + 100);

        // On dark paper, drawn again for it.
        {
            const qucs_s::ink::Paper paper(qucs_s::ink::darkPaperColour());
            QImage img(d->x2 + 10, d->y2 + 40, QImage::Format_RGB32);
            img.fill(qucs_s::ink::darkPaperColour());
            QPainter p(&img);
            d->paintDiagram(&p);
            p.end();
            QVERIFY(coloured(img, plot) > 1000);
        }
    }

    // On a dark plot area (the dark theme's; the automatic one is a white
    // card on a dark canvas too) the traces are in their colour as fitted
    // to it: the picture is drawn again when the paper changes.
    void onDarkPaperInItsColours()
    {
        const QString file = dir.filePath("few.dat");
        QVERIFY(writeDataset(file, waveform(nrz(8), 10e-12, 2.5e-12)));
        QScopedPointer<EyeDiagram> d(makeDiagram(eyeLine(" 1e-10 2 - 2 0.5 1 0 - -")));   // traces, few: opaque
        d->loadGraphData(file);
        auto near = [](const QImage& img, QColor c) {
            int n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor p = img.pixelColor(x, y);
                    if (std::abs(p.red() - c.red()) + std::abs(p.green() - c.green()) + std::abs(p.blue() - c.blue()) < 12) ++n;
                }
            return n;
        };
        const QColor light = QColor(0x00, 0x50, 0xc8);
        QVERIFY2(near(render(d.data(), d->x2 + 10), light) > 50, "the traces in their colour on white");
        d->setTheme(qucs_s::diagramtheme::preset(qucs_s::diagramtheme::Preset::Dark));
        const QColor inside = d->colors().inside;
        QVERIFY(qucs_s::ink::isDark(inside));
        QColor onDark;
        {
            const qucs_s::ink::Paper on(inside);
            onDark = qucs_s::ink::on(light);
        }
        QVERIFY(std::abs(onDark.lightness() - light.lightness()) > 30);
        const QImage img = render(d.data(), d->x2 + 10);
        QVERIFY2(near(img, onDark) > 50 && near(img, light) < 10, qPrintable(QString("%1 %2").arg(near(img, onDark)).arg(near(img, light))));
    }

    // The cursor over it on the canvas: the readout of the eye.
    void theCanvasReadsItOut()
    {
        const QString file = dir.filePath("canvas.dat");
        QVERIFY(writeDataset(file, waveform(nrz(100), 10e-12, 2.5e-12)));
        auto* d = makeDiagram(eyeLine(" 1e-10 2 - 2 - 0 1 - -"));
        d->cx = 100;
        d->cy = 400;
        d->loadGraphData(file);
        doc->a_DocDiags.push_back(d);
        QSignalSpy said(doc, &Schematic::signalCursorPosChanged);
        const QPoint at = doc->modelToViewport(QPoint(d->cx + d->x2 / 4, d->cy - d->y2 / 2));
        QMouseEvent move(QEvent::MouseMove, at, doc->viewport()->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(doc->viewport(), &move);
        QVERIFY(!said.isEmpty());
        const QString text = said.last().at(2).toString();
        QVERIFY2(text.contains("UI)") && text.startsWith("t "), qPrintable(text));
        doc->a_DocDiags.erase(std::find(doc->a_DocDiags.begin(), doc->a_DocDiags.end(), d));
        delete d;
    }

    // The density: a trace counts a pixel once, however many points it has
    // in it - a simulator's short steps at a corner are no brighter spot.
    void aTraceCountsEachPixelOnce()
    {
        // A clock, every corner twenty points (or one).
        auto clock = [](int points) {
            ds::Curve c;
            for (int k = 0; k < 200; ++k) {
                const double v = k % 2, t0 = k * UI;
                for (int i = 0; i < points; ++i) {
                    c.x << t0 + i * 1e-16;
                    c.y << v;
                }
                for (double t = t0 + 2.5e-12; t < t0 + UI - 1e-13; t += 2.5e-12) {
                    c.x << t;
                    c.y << v;
                }
            }
            return c;
        };
        QImage images[2];
        for (int i = 0; i < 2; ++i) {
            const QString file = dir.filePath(QStringLiteral("clock%1.dat").arg(i));
            QVERIFY(writeDataset(file, clock(i == 0 ? 1 : 20)));
            QScopedPointer<EyeDiagram> d(makeDiagram(eyeLine(" 1e-10 2 - 2 0.5 0 0 - -")));
            d->loadGraphData(file);
            images[i] = render(d.data(), d->x2 + 10);
        }
        // Drawn alike.
        int differ = 0, drawn = 0;
        for (int y = 0; y < images[0].height(); ++y)
            for (int x = 0; x < images[0].width(); ++x) {
                const QColor a = images[0].pixelColor(x, y), b = images[1].pixelColor(x, y);
                if (a != Qt::white) ++drawn;
                if (std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) > 30) ++differ;
            }
        QVERIFY2(drawn > 1000 && differ < drawn / 100, qPrintable(QString("%1 of %2").arg(differ).arg(drawn)));
    }

    // The dialog: the eye's group, and what it writes back; a field left as
    // it was shown keeps its value to the last digit.
    void theDialogEditsIt()
    {
        QScopedPointer<EyeDiagram> d(makeDiagram(eyeLine(" 3.33333333333e-10 2 - 2 - 0 1 - -")));
        QVERIFY(d);
        auto* dialog = new DiagramDialog(d.data(), nullptr);   // WA_DeleteOnClose
        auto edit = [&](const QString& placeholder) -> QLineEdit* {
            for (QLineEdit* e : dialog->findChildren<QLineEdit*>())
                if (e->placeholderText() == placeholder) return e;
            return nullptr;
        };
        QLineEdit* ui = nullptr;
        for (QLineEdit* e : dialog->findChildren<QLineEdit*>())
            if (e->toolTip().startsWith("A bit's length")) ui = e;
        QVERIFY(ui != nullptr);
        QLineEdit* start = edit("the start");
        QLineEdit* threshold = edit("halfway between the levels");
        QVERIFY(start && threshold);
        QList<QLineEdit*> mask;
        for (QLineEdit* e : dialog->findChildren<QLineEdit*>())
            if (e->placeholderText() == "no mask") mask << e;
        QCOMPARE(mask.size(), 2);
        QSpinBox* span = nullptr;
        for (QSpinBox* s : dialog->findChildren<QSpinBox*>())
            if (s->suffix() == " UI") span = s;
        QComboBox* levels = nullptr;
        QComboBox* drawn = nullptr;
        for (QComboBox* c : dialog->findChildren<QComboBox*>()) {
            if (c->findText("4 (PAM4)") >= 0) levels = c;
            if (c->findText("traces") >= 0) drawn = c;
        }
        QVERIFY(span && levels && drawn);
        QCheckBox* measure = nullptr;
        for (QCheckBox* c : dialog->findChildren<QCheckBox*>()) {
            QVERIFY(!c->text().startsWith("logarithmic"));
            if (c->text().startsWith("measurements")) measure = c;
        }
        QVERIFY(measure != nullptr);
        if (const QString grab = qEnvironmentVariable("QUCS_TEST_GRAB"); !grab.isEmpty()) {
            auto* tabs = dialog->findChild<QTabWidget*>();
            tabs->setCurrentIndex(1);
            dialog->grab().save(grab + "/eye_dialog.png");
        }

        // Applied as shown: the UI as it was.
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->ui, 3.33333333333e-10);

        QVERIFY(threshold->isEnabled());
        levels->setCurrentIndex(1);
        QVERIFY(!threshold->isEnabled());
        levels->setCurrentIndex(0);
        ui->setText("50 ps");
        span->setValue(3);
        start->setText("2n");
        threshold->setText("450m");
        drawn->setCurrentIndex(1);
        measure->setChecked(false);
        mask.at(0)->setText("0.4");
        mask.at(1)->setText("0.25");
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->ui, 50e-12);
        QCOMPARE(d->span, 3);
        QCOMPARE(d->start, 2e-9);
        QCOMPARE(d->threshold, 0.45);
        QCOMPARE(d->drawn, int(EyeDiagram::Traces));
        QVERIFY(!d->measurements);
        QCOMPARE(d->maskWidth, 0.4);
        QCOMPARE(d->maskHeight, 0.25);
        // Cleared: automatic again; a mask without its height, or wider
        // than a UI, is none.
        ui->setText("");
        mask.at(0)->setText("1.5");
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(std::isnan(d->ui));
        QVERIFY(std::isnan(d->maskWidth) && std::isnan(d->maskHeight));
        dialog->close();
    }

    // The cursor readout: the time into the window, in UI too.
    void theReadoutTellsTheUI()
    {
        const QString file = dir.filePath("readout.dat");
        QVERIFY(writeDataset(file, waveform(nrz(100), 10e-12, 2.5e-12)));
        QScopedPointer<EyeDiagram> d(makeDiagram(eyeLine(" 1e-10 2 - 2 - 0 1 - -")));
        d->loadGraphData(file);
        MappedPoint p{};
        p.x = 50e-12;
        p.y1 = 0.25;
        const QString text = qucs_s::status::readout(d.data(), p);
        QVERIFY2(text.contains("t 50 ps") && text.contains("(0.50 UI)") && text.contains("v 250m"), qPrintable(text));
    }
};

QTEST_MAIN(TestPrbsEye)
#include "test_prbs_eye.moc"
