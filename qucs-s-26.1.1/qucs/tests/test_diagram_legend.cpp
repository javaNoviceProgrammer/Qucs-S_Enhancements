/*
 * The diagram legend (#1719): a box in a chosen corner, or outside on the
 * right, with a sample of every graph's line and its variable. Off by
 * default; the position is saved with the diagram and read back, files
 * without it load as before.
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

// The legend's padding, as diagram.cpp has it.
constexpr qreal legendPadForTest = 4.0;

// A diagram with two graphs (blue solid, red dashed), read from a save
// line; with \a style and \a thick, one graph of that line style and
// thickness alone.
RectDiagram* makeDiagram(const QString& rectLine, int style = -1, int thick = 2)
{
    QString body = style >= 0 ? QStringLiteral("<\"ngspice/ac.v(out)\" #0000ff %1 3 0 %2 0>\n</Rect>\n").arg(thick).arg(style)
                              : QStringLiteral("<\"ngspice/ac.v(out)\" #0000ff 2 3 0 0 0>\n"
                                               "<\"ngspice/ac.i(pr1)\" #ff0000 2 3 0 1 1>\n"
                                               "</Rect>\n");
    QTextStream stream(&body, QIODevice::ReadOnly);
    auto* d = new RectDiagram();
    if (!d->load(rectLine, &stream)) { delete d; return nullptr; }
    return d;
}

// Paints the diagram (box at [0,x2] x [0,y2] of the image, \a wider room on
// its right) and returns it.
QImage render(Diagram* d, int wider = 0)
{
    QImage img(d->x2 + 1 + wider, d->y2 + 1, QImage::Format_RGB32);
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
        QCOMPARE(legend->count(), 6);
        QCOMPARE(legend->itemText(Diagram::LegendOutside), QStringLiteral("outside, on the right"));
        QCOMPARE(legend->currentIndex(), int(Diagram::LegendTopRight));   // shows the current position

        legend->setCurrentIndex(Diagram::LegendBottomRight);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->legendPos, int(Diagram::LegendBottomRight));
        legend->setCurrentIndex(Diagram::LegendOutside);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->legendPos, int(Diagram::LegendOutside));
        legend->setCurrentIndex(Diagram::LegendOff);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->legendPos, int(Diagram::LegendOff));
        dialog->close();
    }

    // Outside: right of all the diagram draws there (its frame, the right
    // axis' numbers), level with its frame's top - none of it inside the
    // frame; within what it is said to draw (the schematic's repaints and
    // scrolling, its card's background); saved and read back.
    void outsideOnTheRight()
    {
        QScopedPointer<RectDiagram> d(makeDiagram(kRectLine));
        QVERIFY(d);
        d->legendPos = Diagram::LegendOutside;
        // (Its axes numbered: the second graph's on the right, its numbers
        // past the frame.)
        d->updateGraphData();
        const QFontMetricsF fm(QFont("Helvetica", 12));
        qreal rightmost = d->x2;
        for (const Text* t : d->Texts)
            rightmost = std::max(rightmost, QTransform().translate(t->x, -t->y).rotate(t->angle()).mapRect(QRectF(0, -fm.ascent(), fm.horizontalAdvance(t->s), fm.height())).right());
        QVERIFY2(rightmost > d->x3 + 10, qPrintable(QString::number(rightmost - d->x3)));
        const QImage out = render(d.data(), 400);
        const QRgb blue = qRgb(0, 0, 255), red = qRgb(255, 0, 0);
        const QRect inside(0, 0, d->x2 + 1, d->y2 + 1), beside(d->x2 + 1, 0, 399, d->y2 + 1);
        QVERIFY(!hasColor(out, inside, blue) && !hasColor(out, inside, red));
        QVERIFY(hasColor(out, beside, blue) && hasColor(out, beside, red));
        const QRectF box = d->legendRect(fm);
        QVERIFY2(box.left() > rightmost, qPrintable(QStringLiteral("%1 %2").arg(box.left()).arg(rightmost)));
        QCOMPARE(box.top(), qreal(-d->y2));
        QVERIFY(d->paintedRect(fm).contains(box.translated(d->cx, d->cy)));
        // A corner's is inside the frame.
        d->legendPos = Diagram::LegendTopRight;
        QVERIFY(d->legendRect(fm).right() < d->x2);
        d->legendPos = Diagram::LegendOutside;
        const QString line = d->save().section('\n', 0, 0);
        QCOMPARE(line.section(' ', 27, 27), QString("5"));
        QScopedPointer<RectDiagram> again(makeDiagram(line));
        QVERIFY(again);
        QCOMPARE(again->legendPos, int(Diagram::LegendOutside));
    }

    // The sample says which line style a trace has. Qt measures a dash
    // pattern in pen widths, so a dashed trace of thickness 2 (its dash 20
    // pixels) and a long-dashed one of any thickness had a single dash
    // across the whole 20-pixel sample: the legend said "solid line" for
    // every style. A trace with point markers was worse, the marker's ring
    // of paper breaking the sample in the middle - so the sample of a
    // solid trace looked dashed and of a dashed one solid (the user's bend
    // study, 8).
    void theSampleShowsTheLineStyle()
    {
        // The ink and the paper along a row's sample: the longest run of
        // paper in it (0: one unbroken line), and how much ink there is.
        const auto along = [](RectDiagram* d, int row, int* ink) {
            const QFontMetricsF fm(QFont("Helvetica", 12));
            const QRectF box = d->legendRect(fm);
            const QImage img = render(d, 0);
            const int mid = int(d->cy + box.y() + legendPadForTest + row * fm.height() + fm.height() / 2.0);
            const int from = int(d->cx + box.x() + legendPadForTest);
            const qreal sample = box.width() - 2 * legendPadForTest - 6.0
                                 - fm.horizontalAdvance(d->Graphs.at(row)->Var);
            int gap = 0, longest = 0;
            *ink = 0;
            for (int x = from + 1; x < from + int(sample); ++x) {
                const QColor c = img.pixelColor(x, mid);
                // (The sample's own colour, or the paper of the box.)
                if (c.red() > 230 && c.green() > 230 && c.blue() > 230) longest = std::max(longest, ++gap);
                else {
                    gap = 0;
                    ++*ink;
                }
            }
            return longest;
        };
        // Solid, thick: one unbroken line. Dashed, dotted and long-dashed,
        // of thickness 1 and 2: broken, each with less ink than the solid
        // one and the dotted one with the least.
        int ink = 0, solidInk = 0;
        for (const int thick : {1, 2}) {
            QScopedPointer<RectDiagram> solid(makeDiagram(kRectLine, 0, thick));
            QVERIFY(solid);
            solid->legendPos = Diagram::LegendTopRight;
            QCOMPARE(along(solid.data(), 0, &solidInk), 0);
            QVERIFY(solidInk > 10);
            for (const int style : {GRAPHSTYLE_DASH, GRAPHSTYLE_DOT, GRAPHSTYLE_LONGDASH}) {
                QScopedPointer<RectDiagram> d(makeDiagram(kRectLine, style, thick));
                QVERIFY(d);
                d->legendPos = Diagram::LegendTopRight;
                const int gap = along(d.data(), 0, &ink);
                const QString what = QStringLiteral("style %1, thickness %2: gap %3, ink %4 of %5").arg(style).arg(thick).arg(gap).arg(ink).arg(solidInk);
                QVERIFY2(gap >= 2, qPrintable(what));
                QVERIFY2(ink > 0 && ink < solidInk, qPrintable(what));
            }
        }
        // A trace with markers: its line is seen on both sides of the
        // marker, so a dashed one is still told from a solid one.
        int withMarker = 0, dashedWithMarker = 0;
        QScopedPointer<RectDiagram> marked(makeDiagram(kRectLine, 0, 2));
        QVERIFY(marked);
        marked->legendPos = Diagram::LegendTopRight;
        marked->Graphs.at(0)->pointMarker = Graph::PointMarker::Circle;
        QVERIFY(marked->Graphs.at(0)->drawsPointMarkers());
        const int plain = along(marked.data(), 0, &withMarker);
        QScopedPointer<RectDiagram> markedDash(makeDiagram(kRectLine, GRAPHSTYLE_DASH, 2));
        QVERIFY(markedDash);
        markedDash->legendPos = Diagram::LegendTopRight;
        markedDash->Graphs.at(0)->pointMarker = Graph::PointMarker::Circle;
        const int dashed = along(markedDash.data(), 0, &dashedWithMarker);
        const QString what = QStringLiteral("with a marker: solid gap %1 ink %2, dashed gap %3 ink %4")
                                 .arg(plain).arg(withMarker).arg(dashed).arg(dashedWithMarker);
        QVERIFY2(dashed > plain + 1, qPrintable(what));
        QVERIFY2(dashedWithMarker < withMarker, qPrintable(what));
        // Which it has room for because a row with a marker gets a longer
        // sample: the line is seen on both sides of it.
        const QFontMetricsF fm(QFont("Helvetica", 12));
        QScopedPointer<RectDiagram> bare(makeDiagram(kRectLine, 0, 2));
        QVERIFY(bare);
        bare->legendPos = Diagram::LegendTopRight;
        const qreal narrow = bare->legendRect(fm).width();
        QVERIFY2(marked->legendRect(fm).width() > narrow + 8,
                 qPrintable(QStringLiteral("%1 against %2").arg(marked->legendRect(fm).width()).arg(narrow)));
    }

    // A label longer than the diagram is high is cut in its middle, its ends
    // kept; one shorter whole (a long expression's ran far above and below
    // the diagram: the update check of 7451b1c). The legend names it whole.
    void longAxisLabelsFitTheDiagram()
    {
        // (Longer than the diagram is high - 300 - in whatever font this
        // has: CI's Linux measured this one's 92 characters no longer, its
        // failure saying nothing more. A font that measures nothing: none.)
        const QFontMetrics metrics(QucsSettings.font, nullptr);
        if (metrics.horizontalAdvance(QLatin1Char('x')) <= 0) QSKIP("no font measures text here");
        QString along = "db(1/(1+j*2*pi*frequency*ac1.r1*1e-9))+20*log10(abs(1/(1+j*2*pi*frequency*ac1.r1*ac1.c1)))";
        while (metrics.boundingRect(along).width() <= 300 && along.size() < 4000) along += QStringLiteral("+abs(ac1.r1)");
        QString body = "<\"ngspice/" + along + "\" #0000ff 2 3 0 0 0>\n<\"ngspice/ac.v(in)\" #ff0000 2 3 0 1 1>\n</Rect>\n";
        QTextStream stream(&body, QIODevice::ReadOnly);
        QScopedPointer<RectDiagram> d(new RectDiagram());
        QVERIFY(d->load(kRectLine, &stream));
        d->updateGraphData();
        QVERIFY2(metrics.boundingRect(along).width() > d->y2,
                 qPrintable(QStringLiteral("%1 px against %2 (%3)").arg(metrics.boundingRect(along).width()).arg(d->y2).arg(QucsSettings.font.toString())));
        int cut = 0, whole = 0;
        for (const Text* t : d->Texts) {
            if (t->angle() == 0) continue;
            QVERIFY2(metrics.boundingRect(t->s).width() <= d->y2, qPrintable(t->s));
            if (t->s.contains(QChar(0x2026))) {
                ++cut;
                QVERIFY2(t->s.startsWith("ngspice/db(1/(") && t->s.endsWith(INVALID_STR.right(4)), qPrintable(t->s));
            } else if (t->s.contains("ac.v(in)")) ++whole;
        }
        QCOMPARE(cut, 1);
        QCOMPARE(whole, 1);
        // An axis' own label too.
        d->yAxis.Label = along;
        d->updateGraphData();
        cut = 0;
        for (const Text* t : d->Texts) cut += t->angle() != 0 && t->s.contains(QChar(0x2026)) ? 1 : 0;
        QCOMPARE(cut, 1);
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestDiagramLegend test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_diagram_legend.moc"
