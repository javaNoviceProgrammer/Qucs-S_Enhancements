/*
 * test_histogram_diagram.cpp - the Histogram diagram: the bins it counts
 * the values into (automatic or set, over the values or the x axis'
 * limits), the heights as counts, percentages or densities, the normal
 * fit, the limits and the share between them, saved and loaded with the
 * schematic, drawn, and edited in the diagram dialog.
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
#include <QImage>
#include <QLineEdit>
#include <QPainter>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>

#include <cmath>
#include <numeric>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "schematic.h"
#include "diagrams/diagramdialog.h"
#include "diagrams/histogramdiagram.h"
#include "isolated_settings.h"

namespace {

// A Histogram diagram line of the size given, its fields after the
// decimals (bins, height, flags, lower, upper) as given.
QString diagramLine(const QString& extra = QString())
{
    return QStringLiteral("<Histogram 0 300 400 300 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 -1 0.5 1 315 0 225 0 0 0 0 -1%1 "
                          "\"\" \"\" \"\">")
        .arg(extra);
}

HistogramDiagram* makeDiagram(const QString& line, const QStringList& vars)
{
    QString body;
    for (const QString& v : vars) body += QStringLiteral("<\"%1\" #0050c8 1 3 0 0 0>\n").arg(v);
    body += "</Histogram>\n";
    QTextStream stream(&body, QIODevice::ReadOnly);
    auto* d = new HistogramDiagram();
    if (!d->load(line, &stream)) {
        delete d;
        return nullptr;
    }
    return d;
}

// A dataset with a variable of the values given against a sample index.
bool writeDataset(const QString& file, const QList<QPair<QString, QVector<double>>>& vars)
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QTextStream s(&f);
    s << "<Qucs Dataset " PACKAGE_VERSION ">\n";
    int longest = 0;
    for (const auto& v : vars) longest = std::max(longest, int(v.second.size()));
    s << "<indep sample " << longest << ">\n";
    for (int i = 0; i < longest; ++i) s << i + 1 << "\n";
    s << "</indep>\n";
    for (const auto& [name, values] : vars) {
        s << "<dep " << name << " sample>\n";
        for (int i = 0; i < longest; ++i) s << (i < values.size() ? values.at(i) : std::nan("")) << "\n";
        s << "</dep>\n";
    }
    return true;
}

QImage render(Diagram* d)
{
    QImage img(d->x2 + 1, d->y2 + 1, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setFont(QFont("Helvetica", 10));
    d->paintDiagram(&p);
    return img;
}

int pixelsNear(const QImage& img, const QRect& area, QColor color, int tolerance)
{
    int n = 0;
    const QRect r = area.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (std::abs(c.red() - color.red()) <= tolerance && std::abs(c.green() - color.green()) <= tolerance
                && std::abs(c.blue() - color.blue()) <= tolerance)
                ++n;
        }
    return n;
}

} // namespace

class TestHistogramDiagram : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // Counting: bin k is [low + k w, low + (k+1) w), the top edge in the
    // last bin, the rest outside; the mean and the sample deviation.
    void theValuesAreCounted()
    {
        const QVector<double> v = {0.0, 0.5, 1.0, 1.5, 2.0, 2.0, 3.0, 4.0, -1.0, 9.0};
        const HistogramDiagram::Bars b = HistogramDiagram::bin(v, 0.0, 4.0, 4);
        QCOMPARE(b.counts, QVector<double>({2, 2, 2, 2}));   // 4.0 in the last bin
        QCOMPARE(b.outside, 2);
        QCOMPARE(b.n, 10);
        QCOMPARE(b.width, 1.0);
        QVERIFY(std::abs(b.mean - 2.2) < 1e-12);
        double squares = 0;
        for (double x : v) squares += (x - 2.2) * (x - 2.2);
        QVERIFY(std::abs(b.sigma - std::sqrt(squares / 9)) < 1e-12);
        QCOMPARE(b.min, -1.0);
        QCOMPARE(b.max, 9.0);
        QCOMPARE(HistogramDiagram::bin({}, 0, 1, 5).n, 0);
        // Far outside: counted as outside, whatever the distance.
        const HistogramDiagram::Bars far = HistogramDiagram::bin({1e300, -1e300, 0.5}, 0.0, 1.0, 10);
        QCOMPARE(far.outside, 2);
        QCOMPARE(far.counts.at(5), 1.0);
    }

    // Automatic bins: by the spread (Freedman-Diaconis), the square root of
    // the number when the quartiles meet, one for too few values.
    void theBinsAreChosen()
    {
        QVector<double> uniform;
        for (int i = 0; i < 1000; ++i) uniform << i / 999.0;
        // IQR 0.5, width 2 * 0.5 / 10 = 0.1: 10 bins over [0, 1].
        QCOMPARE(HistogramDiagram::automaticBins(uniform, 0.0, 1.0), 10);
        QVector<double> flat(100, 3.0);
        flat << 4.0;
        QCOMPARE(HistogramDiagram::automaticBins(flat, 3.0, 4.0), 11);   // ceil(sqrt(101))
        QCOMPARE(HistogramDiagram::automaticBins({1.0}, 0.0, 2.0), 1);
        QVector<double> outliers(1000, 0.0);
        for (int i = 0; i < 1000; ++i) outliers[i] = (i % 2) * 1e-6;
        outliers << 1e6;
        QCOMPARE(HistogramDiagram::automaticBins(outliers, 0.0, 1e6), 200);   // at most 200
    }

    // A diagram of dataset values: the bins over them, the heights on the
    // y axis, what the settings make of the heights.
    void aDiagramBinsItsGraphs()
    {
        const QString file = dir.filePath("h.dat");
        QVector<double> fc;
        for (int i = 0; i < 40; ++i) fc << 100.0 + (i % 10);   // 100..109, four of each
        QVERIFY(writeDataset(file, {{"fc", fc}}));
        QScopedPointer<HistogramDiagram> d(makeDiagram(diagramLine(" 10 0 2 - -"), {"fc"}));
        QVERIFY(d);
        QCOMPARE(d->bins, 10);
        d->loadGraphData(file);
        QCOMPARE(d->bars().size(), 1);
        const HistogramDiagram::Bars b = d->bars().first();   // a copy: the next layout makes new bars
        QCOMPARE(b.n, 40);
        QCOMPARE(b.counts, QVector<double>(10, 4.0));
        QCOMPARE(b.low, 100.0);
        QVERIFY(std::abs(b.width - 0.9) < 1e-12);
        QCOMPARE(d->yAxis.max, 4.0);    // the tallest bar
        QCOMPARE(d->yAxis.low, 0.0);    // the axis from 0
        QVERIFY(d->yAxis.up > 4.0);     // with room above
        QVERIFY(d->xAxis.low <= 100.0 && d->xAxis.up >= 109.0);

        // Percent and density: the same bars, other heights.
        d->height = HistogramDiagram::Percent;
        d->updateGraphData();
        QVERIFY(std::abs(d->yAxis.max - 10.0) < 1e-9);   // 4 of 40
        QCOMPARE(d->yAxis.low, 0.0);
        d->height = HistogramDiagram::Density;
        d->updateGraphData();
        QVERIFY(std::abs(d->yAxis.max - 4 / (40 * 0.9)) < 1e-9);
        // The normal fit may stand above the bars: the axis makes room.
        d->height = HistogramDiagram::Counts;
        d->normalFit = true;
        d->bins = 1;
        d->updateGraphData();
        QCOMPARE(d->bars().first().counts, QVector<double>({40}));
        d->bins = 100;
        d->updateGraphData();
        const double peak = 40 * 0.09 / (b.sigma * std::sqrt(2 * 3.14159265358979323846));
        QVERIFY2(std::abs(d->yAxis.max - std::max(4.0, peak)) < 1e-9, qPrintable(QString::number(d->yAxis.max)));

        // Manual x limits: the bins span them; the rest is outside.
        d->bins = 5;
        d->normalFit = false;
        d->xAxis.autoScale = false;
        d->xAxis.limit_min = 102;
        d->xAxis.limit_max = 107;
        d->updateGraphData();
        QCOMPARE(d->bars().first().low, 102.0);
        // 102..106 a bin each, 107 on the top edge in the last one.
        QCOMPARE(d->bars().first().counts, QVector<double>({4, 4, 4, 4, 8}));
        QCOMPARE(d->bars().first().outside, 16);   // 100, 101, 108, 109

        // The limits: the share between them.
        d->xAxis.autoScale = true;
        d->lowerLimit = 101.5;
        d->upperLimit = 105.0;
        d->updateGraphData();
        QCOMPARE(d->bars().first().within, 16);   // 102, 103, 104, 105
    }

    // Two graphs share the bins; a complex value counts by its magnitude,
    // a missing one not at all.
    void graphsShareTheBins()
    {
        const QString file = dir.filePath("two.dat");
        QVERIFY(writeDataset(file, {{"a", {1, 2, 3, 4}}, {"b", {5, 6}}}));
        QScopedPointer<HistogramDiagram> d(makeDiagram(diagramLine(" 5 0 0 - -"), {"a", "b"}));
        QVERIFY(d);
        d->loadGraphData(file);
        QCOMPARE(d->bars().size(), 2);
        QCOMPARE(d->bars().at(0).low, 1.0);
        QCOMPARE(d->bars().at(1).low, 1.0);
        QCOMPARE(d->bars().at(0).n, 4);
        QCOMPARE(d->bars().at(1).n, 2);   // the NaN rows left out
        QCOMPARE(d->bars().at(1).counts.last(), 2.0);   // 5, and 6 on the top edge

        QFile f(dir.filePath("complex.dat"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("<Qucs Dataset " PACKAGE_VERSION ">\n<indep sample 2>\n1\n2\n</indep>\n"
                "<dep z sample>\n3+j4\n0-j2\n</dep>\n");
        f.close();
        QScopedPointer<HistogramDiagram> c(makeDiagram(diagramLine(), {"z"}));
        c->loadGraphData(dir.filePath("complex.dat"));
        QCOMPARE(c->bars().first().min, 2.0);
        QCOMPARE(c->bars().first().max, 5.0);
    }

    // Saved after the common fields, read back; a line without them (a
    // Cartesian diagram's fields) loads with the defaults.
    void theSettingsAreSavedAndLoaded()
    {
        QScopedPointer<HistogramDiagram> d(makeDiagram(diagramLine(" 12 2 1 1432 1751"), {"fc"}));
        QVERIFY(d);
        QCOMPARE(d->bins, 12);
        QCOMPARE(d->height, int(HistogramDiagram::Density));
        QVERIFY(d->normalFit && !d->statistics);
        QCOMPARE(d->lowerLimit, 1432.0);
        QCOMPARE(d->upperLimit, 1751.0);
        const QString saved = d->save();
        QVERIFY2(saved.startsWith("<Histogram ") && saved.contains(" -1 12 2 1 1432 1751 \"\" \"\" \"\">"),
                 qPrintable(saved));
        QScopedPointer<HistogramDiagram> again(makeDiagram(saved.section('\n', 0, 0), {"fc"}));
        QVERIFY(again);
        QCOMPARE(again->save(), saved);

        QScopedPointer<HistogramDiagram> plain(makeDiagram(diagramLine(), {"fc"}));
        QVERIFY(plain);
        QCOMPARE(plain->bins, 0);
        QVERIFY(plain->statistics && !plain->normalFit);
        QVERIFY(std::isnan(plain->lowerLimit) && std::isnan(plain->upperLimit));
        QVERIFY(plain->save().contains(" -1 0 0 2 - - \""));
        QScopedPointer<HistogramDiagram> junk(makeDiagram(diagramLine(" x -3 y z w"), {"fc"}));
        QVERIFY(junk);
        QCOMPARE(junk->bins, 0);
        QCOMPARE(junk->height, int(HistogramDiagram::Counts));
    }

    // In a schematic: loaded by its tag and saved as it was.
    void aSchematicKeepsIt()
    {
        const QString file = dir.filePath("hist.sch");
        const QString line = diagramLine(" 0 0 3 - 105");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n"
                               "</Symbol>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n  %1\n"
                               "\t<\"fc\" #0050c8 1 3 0 0 0>\n  </Histogram>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
                    .arg(line)
                    .toUtf8());
        f.close();
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        QCOMPARE(doc.a_DocDiags.size(), size_t(1));
        auto* d = dynamic_cast<HistogramDiagram*>(doc.a_DocDiags.front());
        QVERIFY(d != nullptr);
        QCOMPARE(d->upperLimit, 105.0);
        QVERIFY(d->normalFit && d->statistics);
        QVERIFY(doc.save() >= 0);
        QFile back(file);
        QVERIFY(back.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(back.readAll()).contains(line.mid(0, line.size() - 1)), "the line survives");
    }

    // Drawn: the bars in the graph's colour, the limit lines, the box.
    void itIsDrawn()
    {
        const QString file = dir.filePath("draw.dat");
        QVector<double> v;
        for (int i = 0; i < 200; ++i) v << std::sin(i * 0.37) + std::sin(i * 0.11);
        QVERIFY(writeDataset(file, {{"v", v}}));
        QScopedPointer<HistogramDiagram> d(makeDiagram(diagramLine(" 0 0 2 - 1"), {"v"}));
        QVERIFY(d);
        d->loadGraphData(file);
        const QImage withBox = render(d.data());
        // The bars: the blue at 110/255 over white.
        const QColor bar(255 - (255 - 0x00) * 110 / 255, 255 - (255 - 0x50) * 110 / 255, 255 - (255 - 0xc8) * 110 / 255);
        QVERIFY2(pixelsNear(withBox, withBox.rect(), bar, 6) > 2000,
                 qPrintable(QString::number(pixelsNear(withBox, withBox.rect(), bar, 6))));
        // The upper limit: a red line (antialiased, dashed).
        int reddish = 0;
        for (int y = 0; y < withBox.height(); ++y)
            for (int x = 0; x < withBox.width(); ++x) {
                const QColor c = withBox.pixelColor(x, y);
                if (c.red() - std::max(c.green(), c.blue()) > 60) ++reddish;
            }
        QVERIFY2(reddish > 50, qPrintable(QString::number(reddish)));
        // The statistics box in the top right corner: without it the corner
        // holds fewer dark (text) pixels.
        const QRect corner(d->x2 / 2, 0, d->x2 / 2, 30);
        d->statistics = false;
        const QImage without = render(d.data());
        QVERIFY(pixelsNear(withBox, corner, Qt::black, 60) > pixelsNear(without, corner, Qt::black, 60) + 20);
    }

    // The dialog: the histogram's group, and what it writes back.
    void theDialogEditsIt()
    {
        const QString file = dir.filePath("dlg.dat");
        QVERIFY(writeDataset(file, {{"fc", {1, 2, 3, 4, 5}}}));
        QScopedPointer<HistogramDiagram> d(makeDiagram(diagramLine(), {"fc"}));
        QVERIFY(d);
        auto* dialog = new DiagramDialog(d.data(), nullptr);   // WA_DeleteOnClose
        QSpinBox* bins = nullptr;
        for (QSpinBox* s : dialog->findChildren<QSpinBox*>())
            if (s->specialValueText() == "automatic") bins = s;
        QVERIFY(bins != nullptr);
        QComboBox* height = nullptr;
        for (QComboBox* c : dialog->findChildren<QComboBox*>())
            if (c->findText("probability density") >= 0) height = c;
        QVERIFY(height != nullptr);
        QCheckBox* fit = nullptr;
        for (QCheckBox* c : dialog->findChildren<QCheckBox*>())
            if (c->text().startsWith("normal distribution")) fit = c;
        QVERIFY(fit != nullptr);
        QList<QLineEdit*> limits;
        for (QLineEdit* e : dialog->findChildren<QLineEdit*>())
            if (e->placeholderText() == "none") limits << e;
        QCOMPARE(limits.size(), 2);
        // No logarithmic axes, no right axis label.
        for (QCheckBox* c : dialog->findChildren<QCheckBox*>()) QVERIFY(!c->text().startsWith("logarithmic"));
        // For a look: QUCS_TEST_GRAB=<dir> saves the properties tab.
        if (const QString grab = qEnvironmentVariable("QUCS_TEST_GRAB"); !grab.isEmpty()) {
            auto* tabs = dialog->findChild<QTabWidget*>();
            tabs->setCurrentIndex(1);
            dialog->grab().save(grab + "/histogram_dialog.png");
        }

        bins->setValue(7);
        height->setCurrentIndex(HistogramDiagram::Percent);
        fit->setChecked(true);
        limits.at(0)->setText("1.5k");
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->bins, 7);
        QCOMPARE(d->height, int(HistogramDiagram::Percent));
        QVERIFY(d->normalFit);
        QCOMPARE(d->lowerLimit, 1500.0);
        QVERIFY(std::isnan(d->upperLimit));
        dialog->close();
    }
};

QTEST_MAIN(TestHistogramDiagram)
#include "test_histogram_diagram.moc"
