/*
 * test_stacked_diagram.cpp - stacked panes: Cartesian plots one above the
 * other on one x axis. Each pane's own y axes (scaled to its traces alone,
 * or manual and then clipping them), its traces drawn in its band, a
 * marker reading every pane where it is, a zoom box setting the x axis
 * and its own pane's, the cursor readout of the pane under it, saved and
 * loaded with the schematic, and edited in the diagram dialog.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QComboBox>
#include <QImage>
#include <QPainter>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>

#include <cmath>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "statusbar.h"
#include "diagrams/diagramdialog.h"
#include "diagrams/marker.h"
#include "diagrams/stackeddiagram.h"
#include "isolated_settings.h"

namespace {

// A dataset of a, b and c against time 0 to 10: a = t, b = 100 + 1000 t,
// c = -t.
bool writeDataset(const QString& file)
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QTextStream s(&f);
    s << "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 11>\n";
    for (int i = 0; i <= 10; ++i) s << i << "\n";
    s << "</indep>\n<dep a time>\n";
    for (int i = 0; i <= 10; ++i) s << i << "\n";
    s << "</dep>\n<dep b time>\n";
    for (int i = 0; i <= 10; ++i) s << 100 + 1000 * i << "\n";
    s << "</dep>\n<dep c time>\n";
    for (int i = 0; i <= 10; ++i) s << -i << "\n";
    s << "</dep>\n";
    return true;
}

Graph* addGraph(StackedDiagram* d, const QString& var, int pane, int axis = 0, QColor color = Qt::blue)
{
    auto* g = new Graph(d, var);
    g->pane = pane;
    g->yAxisNo = axis;
    g->Color = color;
    d->Graphs.append(g);
    return g;
}

// The heights (y up from the frame's bottom) of a graph's points drawn.
QList<float> heightsOf(Graph* g)
{
    QList<float> ys;
    for (auto p = g->begin(); p != g->end() && !p->isGraphEnd(); ++p)
        if (p->isPt()) ys << p->getScrY();
    return ys;
}

QImage render(Diagram* d)
{
    QImage img(d->x2 + 200, d->y2 + 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setFont(QFont("Helvetica", 10));
    p.translate(100 - d->cx, 50 + d->y2 - d->cy);
    d->paintDiagram(&p);
    return img;
}

} // namespace

class TestStackedDiagram : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString data;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        data = dir.filePath("panes.dat");
        QVERIFY(writeDataset(data));
    }

    // Pane 0 at the top, the gap between two, the pane at a height.
    void thePanesAreLaidOut()
    {
        StackedDiagram d(0, 400);
        d.y2 = 300;
        d.setPaneCount(3);
        const int h = d.paneHeight();
        QCOMPARE(h, (300 - 2 * StackedDiagram::Gap) / 3);
        QCOMPARE(d.paneBottom(2), 0);
        QCOMPARE(d.paneBottom(1), h + StackedDiagram::Gap);
        QCOMPARE(d.paneBottom(0), 2 * (h + StackedDiagram::Gap));
        QCOMPARE(d.paneAt(5), 2);
        QCOMPARE(d.paneAt(d.paneBottom(0) + 1), 0);
        QCOMPARE(d.paneAt(h + StackedDiagram::Gap / 2.0), -1);   // between two
        QCOMPARE(StackedDiagram().paneCount(), 2);                // a new one
        d.setPaneCount(99);
        QCOMPARE(d.paneCount(), StackedDiagram::MaxPanes);
        d.setPaneCount(0);
        QCOMPARE(d.paneCount(), 1);
    }

    // Each pane's y axis is scaled to its own traces, and they are drawn in
    // its band; the x axis is one.
    void eachPaneIsScaledToItsTraces()
    {
        StackedDiagram d(0, 400);
        Graph* a = addGraph(&d, "a", 0);
        Graph* b = addGraph(&d, "b", 1);
        Graph* c = addGraph(&d, "c", 1, 1, Qt::red);
        d.loadGraphData(data);
        QVERIFY(a->cPointsY && b->cPointsY && c->cPointsY);
        // Pane 0: a alone, 0 to 10 (not b's thousands); pane 1: b on the
        // left, c on the right.
        QVERIFY2(d.pane(0).left.up <= 20 && d.pane(0).left.low >= -5, qPrintable(QString::number(d.pane(0).left.up)));
        QVERIFY(d.pane(1).left.up >= 10100 && d.pane(1).left.low > -2000);
        QVERIFY(d.pane(1).right.low <= -10 && d.pane(1).right.up <= 5);
        QCOMPARE(d.pane(0).right.numGraphs, 0);
        QCOMPARE(d.xAxis.low, 0.0);
        QCOMPARE(d.xAxis.up, 10.0);
        const int h = d.paneHeight();
        for (const auto& [g, pane] : {std::pair<Graph*, int>{a, 0}, {b, 1}, {c, 1}}) {
            const QList<float> ys = heightsOf(g);
            QCOMPARE(ys.size(), 11);
            for (float y : ys)
                QVERIFY2(y >= d.paneBottom(pane) - 0.5 && y <= d.paneBottom(pane) + h + 0.5,
                         qPrintable(QStringLiteral("%1 at %2 of pane %3").arg(g->Var).arg(y).arg(pane)));
        }
        // Each pane its frame, and the graphs' names beside their panes.
        int frames = 0;
        for (const qucs::Line* l : d.Lines)
            if (l->part == static_cast<unsigned char>(Diagram::Part::Frame)) ++frames;
        QCOMPARE(frames, 8);
        QStringList left;
        for (const Text* t : d.Texts)
            if (t->s == "a" || t->s == "b" || t->s == "c") left << t->s;
        QCOMPARE(left.size(), 3);
        // Moved to the top pane, b leaves the bottom one's left axis empty,
        // and the top one's takes b's thousands.
        b->pane = 0;
        d.recalcGraphData();
        QCOMPARE(d.pane(1).left.numGraphs, 0);
        QCOMPARE(d.pane(0).left.numGraphs, 2);
        QVERIFY(d.pane(0).left.up >= 10100);
    }

    // A pane's manual limits clip its traces to its band, never into
    // another pane's.
    void aManualPaneClipsItsTraces()
    {
        StackedDiagram d(0, 400);
        addGraph(&d, "a", 0);
        Graph* b = addGraph(&d, "b", 1);
        d.pane(1).left.autoScale = false;
        d.pane(1).left.limit_min = 2000;
        d.pane(1).left.limit_max = 5000;
        d.pane(1).left.step = 1000;
        d.loadGraphData(data);
        const QList<float> ys = heightsOf(b);
        QVERIFY(!ys.isEmpty());
        for (float y : ys) QVERIFY2(y >= -0.5 && y <= d.paneHeight() + 0.5, qPrintable(QString::number(y)));
        QVERIFY(ys.size() < 11);   // some outside, cut off
    }

    // A marker on one pane's trace reads every pane's traces at its x.
    void aMarkerReadsEveryPane()
    {
        StackedDiagram d(0, 400);
        addGraph(&d, "a", 0);
        Graph* b = addGraph(&d, "b", 1);
        addGraph(&d, "c", 1, 1, Qt::red);
        d.loadGraphData(data);
        // On b's sample at time 4.
        const double x = 4, y[2] = {4100, 0};
        float px = 0, py = 0;
        d.calcCoordinate(&x, y, nullptr, &px, &py, d.graphAxis(b));
        QVERIFY(py >= d.paneBottom(1) && py <= d.paneBottom(1) + d.paneHeight());
        auto* m = new Marker(b, 0, int(px), -int(py));
        b->Markers.append(m);
        QVERIFY2(m->Text.contains("b: 4.100k"), qPrintable(m->Text));   // (in the diagram's notation)
        QVERIFY2(m->Text.contains("a: 4") && m->Text.contains("c: -4"), qPrintable(m->Text));
        QCOMPARE(m->Text.count("b: "), 1);
        // Between two samples, straight between them.
        QCOMPARE(d.valuesAt(4.5, 0), QStringList{"a: 4.5"});
        QCOMPARE(d.valuesAt(42, 0), QStringList{"a: -"});
        // A line across both panes where it is.
        const QImage img = render(&d);
        const int col = 100 + m->cx;
        int dark = 0;
        for (int i = 0; i < 2; ++i) {
            const int row = 50 + d.y2 - (d.paneBottom(i) + d.paneHeight() / 2);
            for (int dy = -6; dy <= 6; ++dy)
                if (qGray(img.pixel(col, row + dy)) < 160) {
                    ++dark;
                    break;
                }
        }
        QCOMPARE(dark, 2);
    }

    // A box zooms the x axis and the y axes of the pane it is in.
    void aZoomBoxSetsXAndItsPane()
    {
        StackedDiagram d(0, 400);
        addGraph(&d, "a", 0);
        addGraph(&d, "b", 1);
        d.loadGraphData(data);
        const int b1 = d.paneBottom(1), h = d.paneHeight();
        const QRectF box(QPointF(d.x2 * 0.2, b1 + h * 0.25), QPointF(d.x2 * 0.6, b1 + h * 0.75));
        d.setLimitsBySelectionRect(box);
        QVERIFY(!d.xAxis.autoScale);
        QVERIFY(std::abs(d.xAxis.limit_min - 2) < 1e-9 && std::abs(d.xAxis.limit_max - 6) < 1e-9);
        QVERIFY(!d.pane(1).left.autoScale);
        QVERIFY(d.pane(1).left.limit_min < d.pane(1).left.limit_max);
        QVERIFY(d.pane(0).left.autoScale);   // the other pane: as it was
    }

    // The cursor readout: the pane under it, its traces and axes.
    void theCursorReadsThePaneUnderIt()
    {
        StackedDiagram d(0, 400);
        addGraph(&d, "a", 0);
        addGraph(&d, "b", 1);
        d.loadGraphData(data);
        const MappedPoint top = d.pointToValue(QPointF(d.x2 / 2.0, d.paneBottom(0) + d.paneHeight() / 2.0));
        QCOMPARE(top.pane, 0);
        QVERIFY(std::abs(top.x - 5) < 1e-9);
        const QString readTop = qucs_s::status::readout(&d, top);
        QVERIFY2(readTop.contains("a ") && !readTop.contains("b "), qPrintable(readTop));
        const MappedPoint bottom = d.pointToValue(QPointF(d.x2 / 2.0, d.paneHeight() / 2.0));
        QCOMPARE(bottom.pane, 1);
        QVERIFY(bottom.y1 > 1000);
        const QString readBottom = qucs_s::status::readout(&d, bottom);
        QVERIFY2(readBottom.contains("b ") && !readBottom.contains("a "), qPrintable(readBottom));
    }

    // Saved with the schematic and read back: the panes, their axes (a
    // label with a space, a quote and a percent sign), each trace's pane.
    void itIsSavedAndLoaded()
    {
        StackedDiagram d(0, 400);
        d.setPaneCount(3);
        d.pane(1).left.Label = "I in %, \"ref\"";
        d.pane(2).right.log = true;
        d.pane(2).right.autoScale = false;
        d.pane(2).right.limit_min = 1e-3;
        d.pane(2).right.limit_max = 123.456789012345;
        d.pane(0).left.Units = Axis::dbUnits;
        addGraph(&d, "a", 0);
        addGraph(&d, "b", 2, 1);
        const QString text = d.save();
        QVERIFY2(text.contains("<\"b\" #0000ff 0 3 0 0 1 0 0 0 2>"), qPrintable(text));
        QVERIFY2(text.contains("<\"a\" #0000ff 0 3 0 0 0>"), qPrintable(text));   // the top pane: as before
        QString rest = text.section('\n', 1);
        QTextStream stream(&rest, QIODevice::ReadOnly);
        StackedDiagram e;
        QVERIFY(e.load(text.section('\n', 0, 0), &stream));
        QCOMPARE(e.paneCount(), 3);
        QCOMPARE(e.pane(1).left.Label, QString("I in %, \"ref\""));
        QVERIFY(e.pane(2).right.log && !e.pane(2).right.autoScale);
        QCOMPARE(e.pane(2).right.limit_max, 123.456789012345);
        QCOMPARE(e.pane(0).left.Units, int(Axis::dbUnits));
        QCOMPARE(e.Graphs.size(), 2);
        QCOMPARE(e.Graphs.at(1)->pane, 2);
        QCOMPARE(e.Graphs.at(1)->yAxisNo, 1);
        // A copy keeps it.
        std::unique_ptr<Graph> copy(e.Graphs.at(1)->sameNewOne());
        QCOMPARE(copy->pane, 2);
        // Fewer panes: the traces below go to the last.
        e.setPaneCount(2);
        QCOMPARE(e.Graphs.at(1)->pane, 1);
    }

    // The dialog: the panes' count and table, and each trace's pane.
    void theDialogEditsThePanes()
    {
        StackedDiagram d(0, 400);
        addGraph(&d, "a", 0);
        addGraph(&d, "b", 1);
        d.loadGraphData(data);
        auto* dialog = new DiagramDialog(&d, nullptr);   // WA_DeleteOnClose
        auto* count = dialog->findChild<QSpinBox*>("paneCount");
        auto* table = dialog->findChild<QTableWidget*>("paneTable");
        auto* axisBox = dialog->findChild<QComboBox*>("yAxisBox");
        QVERIFY(count && table && axisBox);
        QCOMPARE(count->value(), 2);
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(axisBox->count(), 4);   // two panes, two sides
        QCOMPARE(axisBox->itemText(3), QString("pane 2, right"));
        if (const QString grab = qEnvironmentVariable("QUCS_TEST_GRAB"); !grab.isEmpty()) {
            auto* tabs = dialog->findChild<QTabWidget*>();
            tabs->setCurrentIndex(1);
            dialog->grab().save(grab + "/stacked_dialog.png");
        }
        count->setValue(3);
        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(axisBox->count(), 6);
        table->item(2, 0)->setText("current");
        table->item(2, 1)->setText("1");
        table->item(2, 2)->setText("5");
        table->item(1, 7)->setCheckState(Qt::Checked);
        table->item(1, 5)->setText("-3");   // from without a to: automatic
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d.paneCount(), 3);
        QCOMPARE(d.pane(2).left.Label, QString("current"));
        QVERIFY(!d.pane(2).left.autoScale);
        QCOMPARE(d.pane(2).left.limit_min, 1.0);
        QCOMPARE(d.pane(2).left.limit_max, 5.0);
        QVERIFY(d.pane(1).right.log && d.pane(1).right.autoScale);
        QVERIFY(d.pane(0).left.autoScale);
        dialog->close();
    }

    // A diagram whose x axis cannot be drawn (log through 0) draws nothing,
    // as a Cartesian one; a pane without traces is a frame.
    void nothingDrawnOnAnUnusableXAxis()
    {
        StackedDiagram d(0, 400);
        Graph* a = addGraph(&d, "a", 1);
        d.xAxis.log = true;
        d.loadGraphData(data);
        QVERIFY(heightsOf(a).isEmpty());
        // (Its data dropped, as a Cartesian diagram's: read again.)
        d.xAxis.log = false;
        a->lastLoaded = QDateTime();
        d.loadGraphData(data);
        QCOMPARE(heightsOf(a).size(), 11);
    }
};

QTEST_MAIN(TestStackedDiagram)
#include "test_stacked_diagram.moc"
