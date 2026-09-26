/*
 * A diagram's marker: its label drawn on the paper it is on - not on the
 * canvas's own background, which made it a dark box around black text in
 * the dark theme - and in the colours chosen for its text and background
 * in its dialog, saved with it (and its indicator) and read back; files
 * of before load as they did.
 */
#include <QtTest>
#include <QCheckBox>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>

#include "config.h"
#include "ink.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "schematic.h"
#include "diagrams/graph.h"
#include "diagrams/marker.h"
#include "diagrams/markerdialog.h"
#include "diagrams/rectdiagram.h"

namespace {

const char* kRectLine =
    "<Rect 0 300 600 300 3 #c0c0c0 1 00 1 1e+6 2e+6 1e+7 1 -0.05 0.2 0.66 1 -0.002 0.01 0.02 315 0 225 1 0 0 \"\" \"\" \"\">";

// A diagram with a graph and a marker (\a marker: its save line).
RectDiagram* makeDiagram(const QString& marker)
{
    QString body = "<\"ngspice/ac.v(out)\" #0000ff 2 3 0 0 0>\n  " + marker + "\n</Rect>\n";
    QTextStream stream(&body, QIODevice::ReadOnly);
    auto* d = new RectDiagram();
    if (!d->load(kRectLine, &stream)) {
        delete d;
        return nullptr;
    }
    return d;
}

// Its first marker, of whichever graph.
Marker* markerOf(Diagram* d)
{
    for (Graph* g : d->Graphs)
        if (!g->Markers.isEmpty()) return g->Markers.first();
    return nullptr;
}

const QFont kFont("Helvetica", 12);

// Where the marker's label is in an image the diagram was painted on at
// its place, its frame left out.
QRect labelBox(const Diagram* d, const Marker* m)
{
    const QSize size = QFontMetrics(kFont).size(0, m->Text);
    return QRect(d->cx + m->x1, d->cy + m->y1, size.width(), size.height()).adjusted(2, 2, -2, -2);
}

// The markers painted on \a canvas - white where the diagram is, as the
// light card it is on dark paper - by a painter whose background is the
// canvas's own (as a widget's is).
QImage paintMarkers(Diagram* d, const QColor& canvas, const QColor& paper = Qt::white)
{
    QImage img(700, 400, QImage::Format_RGB32);
    img.fill(canvas);
    QPainter p(&img);
    p.setFont(kFont);
    p.setBackground(canvas);
    const qucs_s::ink::Paper card(paper);
    p.fillRect(QRect(d->cx, d->cy - d->y2, d->x2, d->y2), paper);
    d->paintMarkers(&p);
    return img;
}

int pixelsOf(const QImage& img, const QRect& area, const std::function<bool(QColor)>& is)
{
    int n = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (is(img.pixelColor(x, y))) ++n;
    return n;
}

} // namespace

class TestMarkerColors : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // The label is filled with the paper - the diagram's light card - and
    // written in its ink, whatever the canvas's background is.
    void theLabelIsOnThePaperNotTheCanvas()
    {
        QScopedPointer<RectDiagram> d(makeDiagram("<Mkr 2e+06 100 -100 3 0 0>"));
        QVERIFY(d);
        Marker* m = markerOf(d.data());
        QVERIFY(m != nullptr);
        m->Text = "frequency: 6.0954M\nac.y: -3.006";
        const QColor dark = qucs_s::ink::darkPaperColour();
        const QImage img = paintMarkers(d.data(), dark);
        const QRect box = labelBox(d.data(), m);
        QVERIFY(img.rect().contains(box));
        QCOMPARE(pixelsOf(img, box, [&dark](QColor c) { return c == dark; }), 0);
        QVERIFY(pixelsOf(img, box, [](QColor c) { return c == QColor(Qt::white); }) > box.width() * box.height() / 2);
        QVERIFY(pixelsOf(img, box, [](QColor c) { return qGray(c.rgb()) < 60; }) > 20);   // black text
        QCOMPARE(m->shownFillColor(), QColor(Qt::white));

        // On light paper of another colour (a theme's cream): that paper.
        const QColor cream(0xfd, 0xf6, 0xe3);
        const QImage onCream = paintMarkers(d.data(), cream, cream);
        QVERIFY(pixelsOf(onCream, box, [&cream](QColor c) { return c == cream; }) > box.width() * box.height() / 2);

        // Transparent: nothing filled - the canvas shows through.
        m->transparent = true;
        const QImage seeThrough = paintMarkers(d.data(), dark, dark);
        QVERIFY(pixelsOf(seeThrough, box, [&dark](QColor c) { return c == dark; }) > box.width() * box.height() / 2);
    }

    // As in the dark theme: a schematic on dark paper, its diagram a light
    // card; the marker's label is light too, its text dark.
    void aMarkerOnDarkPaperIsReadable()
    {
        Schematic sch(nullptr, QStringLiteral(QUCS_EXAMPLES_DIR "/templates_ngspice/AC_Passive_analysis.sch"));
        QVERIFY(sch.load());
        Diagram* d = nullptr;
        Marker* m = nullptr;
        for (Diagram* each : sch.a_DocDiags)
            if (m == nullptr && (m = markerOf(each)) != nullptr) d = each;
        QVERIFY(m != nullptr);
        QPalette pal = sch.viewport()->palette();
        const QColor dark = qucs_s::ink::darkPaperColour();
        pal.setColor(sch.viewport()->backgroundRole(), dark);
        sch.viewport()->setPalette(pal);
        sch.viewport()->setFont(kFont);
        sch.resize(1400, 1000);
        sch.show();
        sch.showAll();
        QCoreApplication::processEvents();
        const QImage canvas = sch.viewport()->grab().toImage();
        // The label's middle, in the view.
        const QPoint middle = sch.modelToViewport(QPoint(d->cx + m->x1 + m->x2 / 2, d->cy + m->y1 + m->y2 / 2));
        const QRect around = QRect(middle - QPoint(6, 3), QSize(12, 6)).intersected(canvas.rect());
        QVERIFY(!around.isEmpty());
        QCOMPARE(pixelsOf(canvas, around, [&dark](QColor c) { return c == dark; }), 0);
        QVERIFY(pixelsOf(canvas, around, [](QColor c) { return qGray(c.rgb()) > 200; }) > 0);
    }

    // Colours chosen: drawn, saved after the indicator and read back; the
    // indicator kept too (it was lost on reading); the lines of before
    // unchanged when nothing is chosen; damaged colours read as automatic.
    void chosenColorsAreDrawnSavedAndRead()
    {
        QScopedPointer<RectDiagram> d(makeDiagram("<Mkr 2e+06 100 -100 3 0 0>"));
        Marker* m = markerOf(d.data());
        m->Text = "frequency: 6.0954M\nac.y: -3.006";
        // As before: the fields of old, and no more - read again, the
        // same (without the data, two zeros were added to its place each
        // time).
        const QString before = m->save();
        QCOMPARE(before, QStringLiteral("<Mkr 2e+06 100 -100 3 0 0>"));
        QScopedPointer<RectDiagram> reread(makeDiagram(before));
        QCOMPARE(markerOf(reread.data())->save(), before);
        const QString head = before.chopped(1);   // (without its '>')

        const QColor ink(0x00, 0xa0, 0x00), fill(0xff, 0xe0, 0xa0);
        m->textColor = ink;
        m->fillColor = fill;
        const QImage img = paintMarkers(d.data(), qucs_s::ink::darkPaperColour());
        const QRect box = labelBox(d.data(), m);
        QVERIFY(pixelsOf(img, box, [&fill](QColor c) { return c == fill; }) > box.width() * box.height() / 2);
        QVERIFY(pixelsOf(img, box, [&ink](QColor c) { return c == ink; }) > 5);
        QCOMPARE(m->save(), head + " 2 #00a000 #ffe0a0>");

        QScopedPointer<RectDiagram> again(makeDiagram(m->save()));
        Marker* read = markerOf(again.data());
        QVERIFY(read != nullptr);
        QCOMPARE(read->textColor, ink);
        QCOMPARE(read->fillColor, fill);
        QCOMPARE(read->indicatorMode, indicator_Triangle);
        QCOMPARE(read->save(), m->save());

        // See-through background, text automatic.
        m->textColor = QColor();
        m->fillColor = QColor(255, 0, 0, 128);
        QCOMPARE(m->save(), head + " 2 - #80ff0000>");
        QScopedPointer<RectDiagram> alpha(makeDiagram(m->save()));
        QVERIFY(!markerOf(alpha.data())->textColor.isValid());
        QCOMPARE(markerOf(alpha.data())->fillColor, QColor(255, 0, 0, 128));

        // The indicator alone.
        m->fillColor = QColor();
        m->indicatorMode = indicator_Square;
        QCOMPARE(m->save(), head + " 1>");
        QScopedPointer<RectDiagram> square(makeDiagram(m->save()));
        QCOMPARE(markerOf(square.data())->indicatorMode, indicator_Square);
        QVERIFY(!markerOf(square.data())->fillColor.isValid());

        // A copy (a diagram copied) keeps them.
        m->textColor = ink;
        std::unique_ptr<Marker> copy(m->sameNewOne(d->Graphs.first()));
        QCOMPARE(copy->indicatorMode, indicator_Square);
        QCOMPARE(copy->textColor, ink);

        // Of before, and damaged.
        QScopedPointer<RectDiagram> old(makeDiagram("<Mkr 1 0 0 3 0 0>"));
        QCOMPARE(markerOf(old.data())->indicatorMode, indicator_Triangle);
        QVERIFY(!markerOf(old.data())->textColor.isValid());
        QScopedPointer<RectDiagram> damaged(makeDiagram("<Mkr 1 0 0 3 0 1 7 notacolor #12>"));
        QVERIFY(damaged && markerOf(damaged.data()) != nullptr);
        QCOMPARE(markerOf(damaged.data())->indicatorMode, indicator_Triangle);
        QVERIFY(!markerOf(damaged.data())->textColor.isValid());
        QVERIFY(!markerOf(damaged.data())->fillColor.isValid());
        QVERIFY(markerOf(damaged.data())->transparent);
    }

    // The dialog: the colours shown, chosen, set back to automatic; the
    // background's disabled when it is transparent; the transparent check
    // box in sight (the buttons covered it); the choice given to the
    // marker.
    void theDialogChoosesThem()
    {
        QScopedPointer<RectDiagram> d(makeDiagram("<Mkr 2e+06 100 -100 3 0 0>"));
        Marker* m = markerOf(d.data());
        QPointer<MarkerDialog> dlg = new MarkerDialog(m);
        dlg->show();
        QVERIFY(QTest::qWaitForWindowExposed(dlg));
        auto* text = dlg->findChild<QPushButton*>("markerTextColor");
        auto* textAuto = dlg->findChild<QToolButton*>("markerTextColorAuto");
        auto* fill = dlg->findChild<QPushButton*>("markerFillColor");
        auto* fillAuto = dlg->findChild<QToolButton*>("markerFillColorAuto");
        auto* transparent = dlg->findChild<QCheckBox*>("markerTransparent");
        QVERIFY(text && textAuto && fill && fillAuto && transparent);
        QCOMPARE(text->text(), QStringLiteral("Automatic"));
        QVERIFY(!textAuto->isEnabled());
        QVERIFY(transparent->isVisible());
        for (QPushButton* b : dlg->findChildren<QPushButton*>())
            if (b->text() == "OK" || b->text() == "Cancel")
                QVERIFY2(!b->geometry().intersects(transparent->geometry()), qPrintable(b->text()));

        dlg->setTextColor(QColor(0x00, 0xa0, 0x00));
        QCOMPARE(text->text(), QStringLiteral("#00A000"));
        QVERIFY(textAuto->isEnabled());
        dlg->setFillColor(QColor(0xff, 0xe0, 0xa0));
        QCOMPARE(fill->text(), QStringLiteral("#FFE0A0"));
        textAuto->click();
        QVERIFY(!dlg->textColor().isValid());
        QCOMPARE(text->text(), QStringLiteral("Automatic"));
        transparent->setChecked(true);
        QVERIFY(!fill->isEnabled());
        QVERIFY(!fillAuto->isEnabled());
        transparent->setChecked(false);
        QVERIFY(fill->isEnabled());

        dlg->setTextColor(QColor(0x12, 0x34, 0x56));
        QVERIFY(QMetaObject::invokeMethod(dlg, "slotAcceptValues"));
        QCOMPARE(dlg->result(), 2);   // changed
        QCOMPARE(m->textColor, QColor(0x12, 0x34, 0x56));
        QCOMPARE(m->fillColor, QColor(0xff, 0xe0, 0xa0));
        QTRY_VERIFY(dlg.isNull());   // (it deletes itself)

        // Nothing changed: 1.
        QPointer<MarkerDialog> same = new MarkerDialog(m);
        QVERIFY(QMetaObject::invokeMethod(same, "slotAcceptValues"));
        QCOMPARE(same->result(), 1);
        QTRY_VERIFY(same.isNull());
    }
};

QTEST_MAIN(TestMarkerColors)
#include "test_marker_colors.moc"
