/*
 * The canvas of a large schematic: an rc ladder of 24,000 components took
 * 170 ms to repaint whatever part of it was on show - every element was
 * drawn, those off the screen too, and half of the time went to laying out
 * names a pixel tall - and each step of a gesture (a selection rectangle,
 * a drag, a wire being drawn) repainted all of it again.
 *
 * Now an element off the painted area is passed over, texts too small to
 * read are left out, and a gesture draws the rest of the schematic once.
 * Checked here: that a part of the canvas painted alone looks as it does
 * in a paint of the whole (nothing passed over that reaches into it, over
 * the examples' symbols); that small texts are left out of the canvas and
 * kept in prints; that a step of a gesture shows what a whole paint would,
 * also after an edit or a selection from elsewhere; and the times.
 *
 *   CANVAS_ALL_EXAMPLES=1  every example, not every fifteenth (minutes)
 *   CANVAS_GRABS=<dir>     the images of the first part that differs
 */
#include <QtTest>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <limits>
#include <random>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "autosave.h"
#include "levelofdetail.h"
#include "mouseactions.h"
#include "schematic.h"
#include "settings.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

void pump()
{
    for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
}

QString schematic(const QString& components, const QString& wires = {})
{
    return QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <View=0,0,800,600,1,0,0>\n"
                          "  <Grid=10,10,0>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n")
           + components + QStringLiteral("</Components>\n<Wires>\n") + wires
           + QStringLiteral("</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
}

QString resistor(const QString& name, int x, int y, int tx = -26, int ty = 15, int rotated = 0)
{
    return QStringLiteral("  <R %1 1 %2 %3 %4 %5 0 %6 \"1k\" 1 \"26.85\" 1 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
        .arg(name).arg(x).arg(y).arg(tx).arg(ty).arg(rotated);
}

// N resistors in rows of 100, joined into one chain by wires (as in
// test_large_schematics).
QString chain(int n)
{
    QString comps = QStringLiteral("  <Vdc V1 1 -60 60 18 -26 0 1 \"5 V\" 1>\n  <GND * 1 -60 90 0 0 0 0>\n");
    QString wires = QStringLiteral("  <-60 30 70 30 \"\" 0 0 0 \"\">\n");
    for (int k = 0; k < n; ++k) {
        const int row = k / 100, col = k % 100;
        const int x = 100 + 100 * col, y = 30 + 100 * row;
        comps += resistor(QStringLiteral("R%1").arg(k + 1), x, y);
        if (col < 99 && k < n - 1) {
            wires += QStringLiteral("  <%1 %2 %3 %2 \"\" 0 0 0 \"\">\n").arg(x + 30).arg(y).arg(x + 70);
        } else if (k < n - 1) {
            wires += QStringLiteral("  <%1 %2 %1 %3 \"\" 0 0 0 \"\">\n").arg(x + 30).arg(y).arg(y + 50);
            wires += QStringLiteral("  <70 %1 %2 %1 \"\" 0 0 0 \"\">\n").arg(y + 50).arg(x + 30);
            wires += QStringLiteral("  <70 %1 70 %2 \"\" 0 0 0 \"\">\n").arg(y + 50).arg(y + 100);
        }
    }
    return schematic(comps, wires);
}

QImage canvasImage(Schematic* s)
{
    QImage image(s->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    s->viewport()->render(&image);
    return image;
}

// The first pixel of \a area where two images differ, if any: by more than
// antialiasing differs by where a paint is clipped (a step or two of a
// colour, where a line crosses the edge of a part painted alone). What is
// not drawn that should be differs by its ink against the paper.
std::optional<QPoint> firstDifference(const QImage& a, const QImage& b, const QRect& area)
{
    const auto near = [](QRgb p, QRgb q) {
        return std::abs(qRed(p) - qRed(q)) <= 8 && std::abs(qGreen(p) - qGreen(q)) <= 8
            && std::abs(qBlue(p) - qBlue(q)) <= 8 && std::abs(qAlpha(p) - qAlpha(q)) <= 8;
    };
    const QRect r = area & a.rect();
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const auto* la = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* lb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x)
            if (!near(la[x], lb[x])) return QPoint(x, y);
    }
    return std::nullopt;
}

// The pixels of \a area (of the canvas) not of the paper's colour.
int inked(const QImage& canvas, const QRect& area, const QColor& paper)
{
    int n = 0;
    const QRect r = area & canvas.rect();
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (canvas.pixelColor(x, y).rgb() != paper.rgb()) ++n;
    return n;
}

QColor paperOf(Schematic* s)
{
    return s->viewport()->palette().color(s->viewport()->backgroundRole());
}

void moveTo(Schematic* s, QPoint at, Qt::MouseButtons buttons)
{
    QMouseEvent e(QEvent::MouseMove, at, s->viewport()->mapToGlobal(at), Qt::NoButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(s->viewport(), &e);
}

} // namespace

class TestCanvasDrawing : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString write(const QString& name, const QString& text)
    {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
        f.write(text.toUtf8());
        return f.fileName();
    }

    // The application with \a file open, shown (off the screen).
    Schematic* open(QucsApp& app, const QString& file, QSize size = {1100, 800})
    {
        app.setAttribute(Qt::WA_DontShowOnScreen);
        app.resize(size);
        app.show();
        pump();
        // An example that asks something as it opens (a Verilog-A model
        // not compiled) is answered no.
        QTimer answer;
        connect(&answer, &QTimer::timeout, [] {
            if (QWidget* dialog = QApplication::activeModalWidget()) dialog->close();
        });
        answer.start(50);
        if (!app.gotoPage(file, false, false)) return nullptr;
        pump();
        return qobject_cast<Schematic*>(app.DocumentTab->currentWidget());
    }

    // Every tile of the canvas, and small squares here and there, painted
    // alone: each as the paint of the whole has it.
    void compareParts(Schematic* s, const QString& what)
    {
        const QImage whole = canvasImage(s);
        const QSize size = whole.size();
        QList<QRect> parts;
        for (int y = 0; y < size.height(); y += 64)
            for (int x = 0; x < size.width(); x += 64) parts << QRect(x, y, 64, 64);
        std::mt19937 rng(qHash(what));
        for (int i = 0; i < 120; ++i)
            parts << QRect(int(rng() % size.width()), int(rng() % size.height()), 5, 5);

        for (const QRect& part : std::as_const(parts)) {
            QImage alone(size, QImage::Format_ARGB32_Premultiplied);
            alone.fill(Qt::transparent);
            s->viewport()->render(&alone, part.topLeft(), QRegion(part));
            if (const auto at = firstDifference(whole, alone, part)) {
                const QString where = QStringLiteral("%1 at scale %2: pixel (%3,%4), in the model (%5,%6), differs when (%7,%8 %9x%10) is painted alone")
                    .arg(what).arg(s->getScale()).arg(at->x()).arg(at->y())
                    .arg(s->viewportToModel(*at).x()).arg(s->viewportToModel(*at).y())
                    .arg(part.x()).arg(part.y()).arg(part.width()).arg(part.height());
                const QString grabs = qEnvironmentVariable("CANVAS_GRABS");
                static bool saved = false;
                if (!grabs.isEmpty() && !saved) {
                    saved = true;
                    whole.save(grabs + "/whole.png");
                    alone.save(grabs + "/alone.png");
                }
                QFAIL(qPrintable(where + QStringLiteral(" (%1 in the whole, %2 alone)")
                                             .arg(whole.pixelColor(*at).name(QColor::HexArgb), alone.pixelColor(*at).name(QColor::HexArgb))));
            }
        }
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.GridMode = 0;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        qucs_s::autosave::setDirectory(dir.filePath("autosave"));
    }

    // A part of the canvas painted alone - only the elements that reach
    // into it drawn - looks as it does in a paint of the whole: texts moved
    // far from their symbols, rotated symbols, and the symbols of the
    // examples (the symbol galleries, and every fifteenth other example),
    // zoomed out and in.
    void aPartOfTheCanvasLooksAsInTheWhole_data()
    {
        QTest::addColumn<QString>("file");
        QString comps;
        comps += resistor("Rfar", 200, 200, 160, -90);
        comps += resistor("Rleft", 420, 200, -150, 40);
        comps += resistor("Rturned", 300, 360, 20, -10, 1);
        comps += resistor("Rturned3", 520, 360, -60, 30, 3);
        comps += QStringLiteral("  <GND * 1 200 260 0 0 0 0>\n");
        comps += QStringLiteral("  <Vac V1 1 100 300 18 -26 0 1 \"1 V\" 1 \"1 GHz\" 1 \"0\" 1 \"0\" 1 \"0\" 1 \"0\" 1>\n");
        const QString wires = QStringLiteral("  <230 200 390 200 \"out\" 330 120 40 \"\">\n"
                                             "  <100 270 100 330 \"\" 0 0 0 \"\">\n"
                                             "  <450 200 450 330 \"\" 0 0 0 \"\">\n");
        QTest::newRow("texts afar") << write("afar.sch", schematic(comps, wires));

        QStringList examples;
        QDirIterator it(QStringLiteral(QUCS_EXAMPLES_DIR), {"*.sch"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) examples << it.next();
        examples.sort();
        for (int i = 0; i < examples.size(); ++i) {
            const bool gallery = examples[i].contains(QLatin1String("/symbols/"));
            if (gallery || i % 15 == 0 || qEnvironmentVariableIsSet("CANVAS_ALL_EXAMPLES"))
                QTest::newRow(qPrintable(QDir(QStringLiteral(QUCS_EXAMPLES_DIR)).relativeFilePath(examples[i])))
                    << examples[i];
        }
    }

    void aPartOfTheCanvasLooksAsInTheWhole()
    {
        QFETCH(QString, file);
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, file, {900, 650});
        if (s == nullptr) QSKIP("does not open without an answer to what it asks");
        s->showAll();
        pump();
        const QString name = QFileInfo(file).fileName();
        compareParts(s, name);
        s->zoomBy(3);
        pump();
        compareParts(s, name);
        s->zoomBy(3);
        pump();
        compareParts(s, name);
        s->setChanged(false);
    }

    // Zoomed out so far that a line of text is under four pixels tall, the
    // canvas leaves the names and values out - the symbols stay - while a
    // print or an export draws them.
    void textsTooSmallToReadAreLeftOut()
    {
        const QString file = write("small.sch", schematic(resistor("R1", 0, 0, 300, -200)));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, file);
        QVERIFY(s != nullptr);
        Component* r = s->a_DocComps.front();
        const int line = QFontMetrics(QucsSettings.font, s->viewport()).lineSpacing();
        const QRect textsInModel(r->cx + r->tx, r->cy + r->ty, 5 * line, 2 * line);

        const auto show = [&](double lineOnScreen) {
            s->showNoZoom();
            s->zoomBy(lineOnScreen / line);
            s->centerOn(QPoint(150, -100));
            pump();
        };
        const auto textsShown = [&](const QImage& canvas) {
            const QRect texts(s->modelToViewport(textsInModel.topLeft()), s->modelToViewport(textsInModel.bottomRight()));
            return inked(canvas, texts, paperOf(s)) > 0;
        };
        const auto symbolShown = [&](const QImage& canvas) {
            const QRect body(s->modelToViewport(r->boundingRect().topLeft()), s->modelToViewport(r->boundingRect().bottomRight()));
            return inked(canvas, body.adjusted(-1, -1, 1, 1), paperOf(s)) > 0;
        };

        show(qucs_s::lod::kSmallestLine + 1.5);
        QImage canvas = canvasImage(s);
        QVERIFY(textsShown(canvas));
        QVERIFY(symbolShown(canvas));

        show(qucs_s::lod::kSmallestLine - 1.5);
        canvas = canvasImage(s);
        QVERIFY2(!textsShown(canvas), "texts too small to read are drawn on the canvas");
        QVERIFY(symbolShown(canvas));

        // A print at the same scale draws them.
        QImage print(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        print.fill(paperOf(s));
        {
            QPainter p(&print);
            p.scale(s->getScale(), s->getScale());
            const QPoint origin = s->viewportToModel(QPoint(0, 0));
            p.translate(-origin.x(), -origin.y());
            p.setRenderHint(QPainter::Antialiasing);
            p.setFont(QucsSettings.font);
            s->paintSchToViewpainter(&p, true);
        }
        QVERIFY2(textsShown(print), "texts left out of a print");
        s->setChanged(false);
    }

    // The editor of a property opens over its text: where a paint puts
    // the text, measured without one (texts too small to read, or off the
    // screen, are not painted).
    void theEditorOpensOverTheTextItEdits()
    {
        QString comps = resistor("R1", 100, 100, -26, 15);
        comps += QStringLiteral("  <R R2 0 300 100 20 30 0 0 \"47k\" 1 \"26.85\" 0 \"0.0\" 1 \"0.0\" 0 \"26.85\" 1 \"european\" 0>\n");
        const QString file = write("editor.sch", schematic(comps));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, file);
        QVERIFY(s != nullptr);
        s->showNoZoom();
        pump();
        auto* hidden = s->a_DocComps.back();
        hidden->showName = false;   // R2's properties start where its name would
        canvasImage(s);
        int checked = 0;
        const auto sim = QucsSettings.DefaultSimulator;
        for (Component* c : s->a_DocComps)
            for (Property* prop : c->Props)
                if (prop->display && (prop->simulators & sim) == sim) {   // (as paint() shows them)
                    QCOMPARE(c->textOrigin(prop), prop->boundingRect().topLeft());
                    ++checked;
                }
        QVERIFY(checked >= 3);
        s->setChanged(false);
    }

    // A step of a selection rectangle shows, around it, the schematic as a
    // paint of the whole does - also after an edit made from elsewhere in
    // the middle of it, and a selection changed from elsewhere.
    void aGestureStepShowsWhatAWholePaintWould()
    {
        const QString file = write("gesture.sch", chain(400));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, file);
        QVERIFY(s != nullptr);
        s->showAll();
        pump();

        // From an empty place below the chain, over empty paper: nothing
        // is selected by it.
        const QPoint from = s->modelToViewport(QPoint(2000, 1500));
        const auto band = [&](QPoint to) { return QRect(from, to).normalized().adjusted(-3, -3, 3, 3); };
        const auto outside = [](const QImage& a, const QImage& b, const QRect& band) {
            for (const QRect& r : (QRegion(a.rect()) - QRegion(band)))
                if (const auto at = firstDifference(a, b, r)) return at;
            return std::optional<QPoint>{};
        };

        QTest::mousePress(s->viewport(), Qt::LeftButton, Qt::NoModifier, from);
        pump();
        const QImage whole = canvasImage(s);

        QPoint to = from + QPoint(90, 12);
        moveTo(s, to, Qt::LeftButton);
        const QImage first = canvasImage(s);
        QVERIFY(!outside(whole, first, band(to)));
        QVERIFY2(inked(first, band(to).adjusted(4, 4, -4, -4), paperOf(s)) > 0, "no selection rectangle drawn");

        to = from + QPoint(140, 20);
        moveTo(s, to, Qt::LeftButton);
        const QImage second = canvasImage(s);
        if (const auto at = outside(whole, second, band(to)))
            QFAIL(qPrintable(QStringLiteral("a step differs from the whole at (%1,%2)").arg(at->x()).arg(at->y())));

        // An edit from elsewhere (Claude's tools edit so): a resistor turned.
        // (R50 and R150 are in the middle of the chain, on show: it is wider
        // than the canvas at the smallest zoom.)
        Component* r = *std::next(s->a_DocComps.begin(), 51);
        QCOMPARE(r->Name, QStringLiteral("R50"));
        r->rotate();
        s->setChanged(true, true);
        moveTo(s, to + QPoint(4, 0), Qt::LeftButton);
        const QImage edited = canvasImage(s);
        const QRect turned(s->modelToViewport(r->boundingRect().topLeft()), s->modelToViewport(r->boundingRect().bottomRight()));
        QVERIFY(s->viewport()->rect().contains(turned));
        QVERIFY2(firstDifference(whole, edited, turned.adjusted(-2, -2, 2, 2)), "an edit from elsewhere not shown");

        // A selection from elsewhere.
        Component* other = *std::next(s->a_DocComps.begin(), 151);
        QCOMPARE(other->Name, QStringLiteral("R150"));
        QVERIFY(s->viewport()->rect().contains(s->modelToViewport(other->center())));
        other->isSelected = true;
        moveTo(s, to + QPoint(8, 0), Qt::LeftButton);
        const QImage selected = canvasImage(s);
        QTest::mouseRelease(s->viewport(), Qt::LeftButton, Qt::NoModifier, to + QPoint(8, 0));
        pump();
        other->isSelected = true;   // (the release selects afresh: what the band holds, nothing)
        const QImage after = canvasImage(s);
        if (const auto at = outside(after, selected, band(to + QPoint(8, 0))))
            QFAIL(qPrintable(QStringLiteral("an edit or a selection from elsewhere not shown, at (%1,%2)").arg(at->x()).arg(at->y())));
        s->setChanged(false);
    }

    // A dragged component is drawn over the rest, held from the step
    // before: each step shows what a paint of the whole would.
    void aDragStepShowsWhatAWholePaintWould()
    {
        QString comps = resistor("Rlone", 600, 600);
        for (int i = 0; i < 30; ++i) comps += resistor(QStringLiteral("R%1").arg(i + 1), 100 + 60 * (i % 10), 100 + 80 * (i / 10));
        const QString file = write("drag.sch", schematic(comps));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, file);
        QVERIFY(s != nullptr);
        s->showAll();
        pump();
        Component* lone = s->a_DocComps.front();
        QCOMPARE(lone->Name, QStringLiteral("Rlone"));
        const QPoint at = s->modelToViewport(lone->center());
        QTest::mouseClick(s->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        pump();
        QVERIFY(lone->isSelected);
        QTest::mousePress(s->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        pump();
        for (int step = 1; step <= 4; ++step) {
            moveTo(s, at + QPoint(0, -25 * step), Qt::LeftButton);
            const QImage stepImage = canvasImage(s);
            const QImage whole = canvasImage(s);   // (the step was painted: this is a whole paint)
            if (const auto differs = firstDifference(whole, stepImage, stepImage.rect()))
                QFAIL(qPrintable(QStringLiteral("step %1 differs from the whole at (%2,%3)").arg(step).arg(differs->x()).arg(differs->y())));
        }
        QVERIFY(lone->center() != s->viewportToModel(at));
        QTest::mouseRelease(s->viewport(), Qt::LeftButton, Qt::NoModifier, at + QPoint(0, -100));
        pump();
        s->setChanged(false);
    }

    // A corner of a large schematic is painted in a fraction of the time
    // the whole of it takes (all 24,000 elements of the user's schematic
    // were drawn, wherever the view was: a corner took longer than the
    // whole, its names drawn too); a step of a gesture over it, zoomed out
    // to show it, takes a fraction of a whole paint. (Fractions of a paint
    // on the same machine, not times: the tests run under the sanitizers
    // too, which slow the looks at thousands of elements down tenfold.)
    void aLargeSchematicIsQuickToWorkOn()
    {
        const auto repaint = [](Schematic* s) {
            qint64 best = std::numeric_limits<qint64>::max();
            for (int i = 0; i < 5; ++i) {
                QElapsedTimer t;
                t.start();
                s->viewport()->repaint();
                best = std::min(best, t.nsecsElapsed());
            }
            return best;
        };

        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, write(QStringLiteral("large.sch"), chain(16000)));
        QVERIFY(s != nullptr);
        s->showNoZoom();
        s->centerOn(QPoint(400, 300));
        pump();
        const qint64 corner = repaint(s);

        s->showAll();
        pump();
        const qint64 whole = repaint(s);
        const QPoint from = s->modelToViewport(QPoint(-40, -60));
        QTest::mousePress(s->viewport(), Qt::LeftButton, Qt::NoModifier, from);
        pump();
        moveTo(s, from + QPoint(10, 10), Qt::LeftButton);
        s->viewport()->repaint();   // (the scene drawn once)
        qint64 step = std::numeric_limits<qint64>::max();
        for (int k = 2; k < 12; ++k) {
            QElapsedTimer t;
            t.start();
            moveTo(s, from + QPoint(10 * k, 6 * k), Qt::LeftButton);
            s->viewport()->repaint();
            step = std::min(step, t.nsecsElapsed());
        }
        QTest::mouseRelease(s->viewport(), Qt::LeftButton, Qt::NoModifier, from + QPoint(120, 72));
        pump();
        s->setChanged(false);

        qInfo("16,000 resistors: %.1f ms a corner at 1:1; zoomed out, %.1f ms a whole paint, %.1f ms a step of a selection rectangle",
              corner / 1e6, whole / 1e6, step / 1e6);
        QVERIFY2(corner * 3 < whole, qPrintable(QStringLiteral("a corner %1 ms, the whole %2 ms")
                                                    .arg(corner / 1e6).arg(whole / 1e6)));
        QVERIFY2(step * 4 < whole, qPrintable(QStringLiteral("a step %1 ms, a whole paint %2 ms")
                                                  .arg(step / 1e6).arg(whole / 1e6)));
    }
};

QTEST_MAIN(TestCanvasDrawing)
#include "test_canvas_drawing.moc"
