/*
 * The diagram legend (#1719): a box in a chosen corner with a sample of
 * every graph's line and its variable. Off by default; the position is
 * saved with the diagram and read back, files without it load as before.
 */
#include <QtTest>
#include <QImage>
#include <QPainter>

#include "config.h"
#include "main.h"
#include "module.h"
#include "misc.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/graph.h"
#include "diagrams/diagramdialog.h"
#include <QComboBox>
#include <QLabel>

namespace {
const char* kRectLine =
    "<Rect 0 300 600 300 3 #c0c0c0 1 00 1 1e+6 2e+6 1e+7 1 -0.05 0.2 0.66 1 -0.002 0.01 0.02 315 0 225 1 0 0 \"\" \"\" \"\">";

// A diagram with two graphs (blue solid, red dashed), read from a save line.
RectDiagram* makeDiagram(const QString& rectLine)
{
    QString body = "<\"ngspice/ac.v(out)\" #0000ff 2 3 0 0 0>\n"
                   "<\"ngspice/ac.i(pr1)\" #ff0000 2 3 0 1 1>\n"
                   "</Rect>\n";
    QTextStream stream(&body, QIODevice::ReadOnly);
    auto* d = new RectDiagram();
    if (!d->load(rectLine, &stream)) { delete d; return nullptr; }
    return d;
}

// Paints the diagram (box at [0,x2] x [0,y2] of the image) and returns it.
QImage render(Diagram* d)
{
    QImage img(d->x2 + 1, d->y2 + 1, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setFont(QFont("Helvetica", 12));
    d->paintDiagram(&p);
    return img;
}

// Whether a pixel of exactly this colour lies inside the rectangle.
bool hasColor(const QImage& img, const QRect& area, QRgb color)
{
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (img.pixel(x, y) == color) return true;
    return false;
}
} // namespace

class TestDiagramLegend : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void offByDefaultAndSavedWithTheDiagram()
    {
        QScopedPointer<RectDiagram> d(makeDiagram(kRectLine));
        QVERIFY(d);
        QCOMPARE(d->legendPos, int(Diagram::LegendOff));
        QCOMPARE(d->Graphs.size(), 2);

        d->legendPos = Diagram::LegendBottomLeft;
        const QString saved = d->save();
        const QString firstLine = saved.section('\n', 0, 0);
        // ...as the field after the units, before the labels; the labels
        // are still the last, quoted, items.
        QCOMPARE(firstLine.section(' ', 27, 27), QString("3"));
        QVERIFY(firstLine.endsWith("\"\" \"\" \"\">"));

        QScopedPointer<RectDiagram> again(makeDiagram(firstLine));
        QVERIFY(again);
        QCOMPARE(again->legendPos, int(Diagram::LegendBottomLeft));
        QCOMPARE(again->Graphs.size(), 2);
        QCOMPARE(again->xAxis.limit_max, 1e7);   // the fields before it still land
        QCOMPARE(again->yAxis.Units, 0);
        QCOMPARE(again->notation, qucs_s::numberformat::Notation::Engineering);
    }

    void olderFilesAndBadValuesLoadWithoutALegend()
    {
        // The example files: no notation/units fields at all.
        QScopedPointer<RectDiagram> old(makeDiagram(
            "<Rect 568 291 413 217 3 #c0c0c0 1 00 1 1e+6 2e+6 1e+7 1 -0.05 0.2 0.66 1 -0.002 0.01 0.02 315 0 225 \"\" \"\" \"\">"));
        QVERIFY(old);
        QCOMPARE(old->legendPos, int(Diagram::LegendOff));
        // Units but no legend field (files of upstream 26.1.1).
        QScopedPointer<RectDiagram> units(makeDiagram(
            "<Rect 568 291 413 217 3 #c0c0c0 1 00 1 1e+6 2e+6 1e+7 1 -0.05 0.2 0.66 1 -0.002 0.01 0.02 315 0 225 1 0 0 \"x label\" \"\" \"\">"));
        QVERIFY(units);
        QCOMPARE(units->legendPos, int(Diagram::LegendOff));
        QCOMPARE(units->xAxis.Label, QString("x label"));
        // Out of range or damaged: off, and the diagram still loads.
        QScopedPointer<RectDiagram> bad(makeDiagram(
            "<Rect 568 291 413 217 3 #c0c0c0 1 00 1 1e+6 2e+6 1e+7 1 -0.05 0.2 0.66 1 -0.002 0.01 0.02 315 0 225 1 0 0 99 \"\" \"\" \"\">"));
        QVERIFY(bad);
        QCOMPARE(bad->legendPos, int(Diagram::LegendOff));
        QScopedPointer<RectDiagram> junk(makeDiagram(
            "<Rect 568 291 413 217 3 #c0c0c0 1 00 1 1e+6 2e+6 1e+7 1 -0.05 0.2 0.66 1 -0.002 0.01 0.02 315 0 225 1 0 0 x \"\" \"\" \"\">"));
        QVERIFY(junk);
        QCOMPARE(junk->legendPos, int(Diagram::LegendOff));
    }

    void drawsTheGraphsSamplesInTheChosenCorner()
    {
        QScopedPointer<RectDiagram> d(makeDiagram(kRectLine));
        QVERIFY(d);
        const QRgb blue = qRgb(0, 0, 255), red = qRgb(255, 0, 0);
        const QRect topRight(d->x2 / 2, 0, d->x2 / 2, d->y2 / 2);
        const QRect topLeft(0, 0, d->x2 / 2, d->y2 / 2);
        const QRect bottomRight(d->x2 / 2, d->y2 / 2, d->x2 / 2, d->y2 / 2);

        // Off: nothing of the graphs' colours anywhere (there is no data).
        QImage off = render(d.data());
        QVERIFY(!hasColor(off, off.rect(), blue));
        QVERIFY(!hasColor(off, off.rect(), red));

        d->legendPos = Diagram::LegendTopRight;
        QImage tr = render(d.data());
        QVERIFY(hasColor(tr, topRight, blue));
        QVERIFY(hasColor(tr, topRight, red));
        QVERIFY(!hasColor(tr, topLeft, blue));
        QVERIFY(!hasColor(tr, bottomRight, blue));

        d->legendPos = Diagram::LegendTopLeft;
        QImage tl = render(d.data());
        QVERIFY(hasColor(tl, topLeft, blue));
        QVERIFY(!hasColor(tl, topRight, blue));

        d->legendPos = Diagram::LegendBottomRight;
        QImage br = render(d.data());
        QVERIFY(hasColor(br, bottomRight, red));
        QVERIFY(!hasColor(br, topRight, red));

        // No graphs: no box either (the corner stays white).
        qDeleteAll(d->Graphs);
        d->Graphs.clear();
        QImage empty = render(d.data());
        QVERIFY(!hasColor(empty, empty.rect(), qRgb(169, 169, 169)));   // the box frame
    }

    // The diagram dialog: a "Legend" box on the properties tab, applied
    // with the other properties.
    void dialogSetsThePosition()
    {
        QScopedPointer<RectDiagram> d(makeDiagram(kRectLine));
        QVERIFY(d);
        d->legendPos = Diagram::LegendTopRight;
        auto* dialog = new DiagramDialog(d.data(), nullptr);   // WA_DeleteOnClose
        QComboBox* legend = nullptr;
        for (QComboBox* box : dialog->findChildren<QComboBox*>())
            if (box->findText("top right") >= 0 && box->findText("bottom left") >= 0) legend = box;
        QVERIFY(legend != nullptr);
        QCOMPARE(legend->count(), 5);
        QCOMPARE(legend->currentIndex(), int(Diagram::LegendTopRight));   // shows the current position

        legend->setCurrentIndex(Diagram::LegendBottomRight);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->legendPos, int(Diagram::LegendBottomRight));
        legend->setCurrentIndex(Diagram::LegendOff);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->legendPos, int(Diagram::LegendOff));
        dialog->close();
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestDiagramLegend test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_diagram_legend.moc"
