/*
 * View > View All with a diagram on the schematic. A marker's bounds were
 * in its diagram's coordinates (its label from the diagram's lower left
 * corner, the point it marks upside down), not the schematic's: each
 * marker added a box near the origin, and View All zoomed out to show it,
 * the schematic small in a corner - and a selection rectangle there
 * selected markers far away. A diagram's bounds also left out its title,
 * and the numbers of its axes centred on the top and right edges of its
 * frame (a title wider than its plot was cut off); and a Smith chart's
 * drawing was that of the circles of its grid, many times its size (its
 * card in the dark theme as large).
 */
#include <QtTest>
#include <QDirIterator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "autosave.h"
#include "schematic.h"
#include "settings.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "diagrams/marker.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "levelofdetail.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

void pump()
{
    for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
}

// A resistor at (1000, 1000) and, below it, a diagram of 500 x 300 with its
// lower left corner at (1200, 1500): a title, and a marker whose label is
// above and right of its frame. All far from the origin.
QString schematicText(const QString& diagrams)
{
    return QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <View=0,0,800,600,1,0,0>\n"
                          "  <Grid=10,10,0>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n"
                          "  <R R1 1 1000 1000 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                          "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n")
         + diagrams + QStringLiteral("</Diagrams>\n<Paintings>\n</Paintings>\n");
}

const QString kPlot = QStringLiteral(
    "  <Rect 1200 1500 500 300 3 #c0c0c0 1 00 0 0 10 100 0 0 5 25 1 -0.1 0.5 1.1 "
    "315 0 225 1 0 0 0 -1 \"x (s)\" \"y (V)\" \"\" \"A title of some length\">\n"
    "    <\"y\" #ff0000 2 3 0 0 0>\n"
    "      <Mkr 50 540 -360 3 0 0>\n"
    "  </Rect>\n");

// A narrow plot with a title much wider than it, centred above it, and no
// marker: its title reaches far left and right of its frame.
const QString kNarrow = QStringLiteral(
    "  <Rect 1200 1500 200 150 3 #c0c0c0 1 00 0 0 10 100 0 0 5 25 1 -0.1 0.5 1.1 "
    "315 0 225 1 0 0 0 -1 \"x (s)\" \"y (V)\" \"\" \"The output voltage of the amplifier over the first 100 seconds\">\n"
    "    <\"y\" #ff0000 2 3 0 0 0>\n"
    "  </Rect>\n");

QString dataset()
{
    QString xs, ys;
    for (int k = 0; k <= 50; ++k) {
        xs += QStringLiteral("  %1\n").arg(2 * k);
        ys += QStringLiteral("  %1\n").arg(0.44 * k);
    }
    return QStringLiteral("<Qucs Dataset " PACKAGE_VERSION ">\n<indep x 51>\n") + xs
         + QStringLiteral("</indep>\n<dep y x>\n") + ys + QStringLiteral("</dep>\n");
}

QImage canvasImage(Schematic* s)
{
    QImage image(s->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    s->viewport()->render(&image);
    return image;
}

// The pixels of the canvas not of the paper's colour: where they are.
QRect inkOf(const QImage& canvas, const QColor& paper)
{
    QRect ink;
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x)
            if (canvas.pixelColor(x, y).rgb() != paper.rgb()) ink |= QRect(x, y, 1, 1);
    return ink;
}

} // namespace

class TestViewAll : public QObject
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

    Schematic* open(QucsApp& app, const QString& file)
    {
        app.setAttribute(Qt::WA_DontShowOnScreen);
        app.resize(1100, 800);
        app.show();
        pump();
        QTimer answer;
        connect(&answer, &QTimer::timeout, [] {
            if (QWidget* dialog = QApplication::activeModalWidget()) dialog->close();
        });
        answer.start(50);
        if (!app.gotoPage(file, false, false)) return nullptr;
        pump();
        auto* s = qobject_cast<Schematic*>(app.DocumentTab->currentWidget());
        if (s != nullptr) s->setGridOn(false);   // (its dots would be ink everywhere)
        return s;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.GridMode = 0;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        QucsSettings.font = QApplication::font();
        Module::registerModules();
        qucs_s::autosave::setDirectory(dir.filePath("autosave"));
        write(QStringLiteral("plot.dat"), dataset());
    }

    // A marker's bounds are where it is drawn on the schematic: its label
    // and the point on the curve it marks.
    void aMarkerIsBoundedWhereItIsDrawn()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, write(QStringLiteral("plot.sch"), schematicText(kPlot)));
        QVERIFY(s != nullptr);
        QCOMPARE(s->a_DocDiags.size(), 1);
        Diagram* d = s->a_DocDiags.front();
        QCOMPARE(d->Graphs.size(), 1);
        QCOMPARE(d->Graphs.front()->Markers.size(), 1);
        const Marker* m = d->Graphs.front()->Markers.front();
        QVERIFY2(m->Text != QObject::tr("invalid"), qPrintable(m->Text));   // (it has its data)

        const QRect bounds = m->boundingRect();
        const QRect label(d->cx + m->x1, d->cy + m->y1, m->x2, m->y2);
        const QPoint marked(d->cx + m->cx, d->cy - m->cy);
        QVERIFY2(bounds.contains(label), qPrintable(QStringLiteral("(%1,%2 %3x%4) holds not its label")
                                                        .arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height())));
        QVERIFY(bounds.contains(marked));
        // The point is on the curve, in the frame; bounds.contains() above
        // would also hold for a box reaching the origin.
        QVERIFY(QRect(d->cx, d->cy - d->y2, d->x2, d->y2).contains(marked));
        QVERIFY2(bounds.left() > 1000 && bounds.top() > 1000,
                 qPrintable(QStringLiteral("(%1,%2)").arg(bounds.left()).arg(bounds.top())));

        // A selection rectangle takes it where it is, not by the origin.
        s->selectElements(QRect(0, 0, 900, 900), false, false);
        QVERIFY(!m->isSelected);
        s->selectElements(bounds.adjusted(-5, -5, 5, 5), false, true);
        QVERIFY(m->isSelected);
        QVERIFY(!d->isSelected);
        s->deselectElements(nullptr);
        s->setChanged(false);
    }

    // A diagram's title - here wider than it, and nothing else above it -
    // and the numbers of its axes are in the used area, and in the bounds
    // of a selection that holds it (Print Selection, Zoom to Selection).
    void aTitleIsInTheUsedArea()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, write(QStringLiteral("narrow.sch"), schematicText(kNarrow)));
        QVERIFY(s != nullptr);
        Diagram* d = s->a_DocDiags.front();
        const QRectF drawn = d->paintedRect(QFontMetricsF(QucsSettings.font));
        QVERIFY(drawn.left() < d->cx - 100 && drawn.right() > d->cx + d->x2 + 100);   // (the title, wider)
        QVERIFY(drawn.top() < d->cy - d->y2 - d->titleHeight() / 2);
        QVERIFY(QRectF(s->allBoundingRect()).contains(drawn));
        d->isSelected = true;
        QVERIFY(QRectF(s->currentSelection().bounds).contains(drawn));
        d->isSelected = false;
        s->setChanged(false);
    }

    // Everything a diagram draws is in the used area, its marker's label
    // too - and nothing else.
    void theUsedAreaHoldsAllADiagramDraws()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, write(QStringLiteral("plot.sch"), schematicText(kPlot)));
        QVERIFY(s != nullptr);
        Diagram* d = s->a_DocDiags.front();
        const Marker* m = d->Graphs.front()->Markers.front();
        const QRect used = s->allBoundingRect();
        const QRectF drawn = d->paintedRect(QFontMetricsF(QucsSettings.font));
        const auto where = [](const QRectF& r) {
            return QStringLiteral("(%1,%2)-(%3,%4)").arg(r.left()).arg(r.top()).arg(r.right()).arg(r.bottom());
        };
        QVERIFY2(QRectF(used).contains(drawn), qPrintable(where(used) + " holds not " + where(drawn)));
        QVERIFY(used.contains(m->boundingRect()));
        QVERIFY(drawn.top() < d->cy - d->y2 - d->titleHeight() / 2);   // (the title is in it)
        // ...and nothing else: the resistor at (1000, 1000) is the top left.
        QVERIFY2(used.left() > 900 && used.top() > 900, qPrintable(where(used)));
        s->setChanged(false);
    }

    // View All shows all of the schematic, filling the window: nothing cut
    // off at an edge, and no room kept for a box by the origin.
    void viewAllShowsItAll_data()
    {
        QTest::addColumn<QString>("diagrams");
        QTest::newRow("a plot with a title and a marker") << kPlot;
        QTest::newRow("a narrow plot with a long title") << kNarrow;
        QTest::newRow("two markers") << QString(kPlot).replace(
            QStringLiteral("      <Mkr 50 540 -360 3 0 0>\n"),
            QStringLiteral("      <Mkr 50 540 -360 3 0 0>\n      <Mkr 90 -200 40 3 0 0>\n"));
    }

    void viewAllShowsItAll()
    {
        QFETCH(QString, diagrams);
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, write(QStringLiteral("plot.sch"), schematicText(diagrams)));
        QVERIFY(s != nullptr);
        s->showAll();
        pump();
        const QImage canvas = canvasImage(s);
        const QColor paper = s->viewport()->palette().color(s->viewport()->backgroundRole());
        const QRect ink = inkOf(canvas, paper);
        QVERIFY(!ink.isEmpty());
        const QString what = QStringLiteral("ink (%1,%2 %3x%4) in a canvas of %5x%6 at scale %7")
                                 .arg(ink.x()).arg(ink.y()).arg(ink.width()).arg(ink.height())
                                 .arg(canvas.width()).arg(canvas.height()).arg(s->getScale());
        // Nothing cut off: the margin View All keeps is on every side.
        QVERIFY2(canvas.rect().adjusted(3, 3, -3, -3).contains(ink), qPrintable(what));
        // It fills the window, one way or the other.
        QVERIFY2(ink.width() > canvas.width() * 3 / 4 || ink.height() > canvas.height() * 3 / 4, qPrintable(what));
        s->setChanged(false);
    }

    // The examples with a diagram - plots, Smith charts, tables, timing
    // diagrams, their data missing - shown whole by View All. (A Smith
    // chart's drawing counted the circles its grid's arcs are parts of,
    // many times its size: all it draws in the used area, View All showed
    // it small in the middle.)
    void viewAllShowsEveryExampleWhole_data()
    {
        QTest::addColumn<QString>("file");
        static const QRegularExpression plot(QStringLiteral("\\n\\s*<Rect "));
        static const QRegularExpression other(QStringLiteral("\\n\\s*<(Polar|Smith|ySmith|PS|SP|Tab|Truth|Time|Curve|Rect3D|Histogram) "));
        QStringList examples, others;
        QDirIterator it(QStringLiteral(QUCS_EXAMPLES_DIR), {"*.sch"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            QFile f(file);
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QString text = QString::fromUtf8(f.readAll());
            if (other.match(text).hasMatch()) others << file;
            else if (plot.match(text).hasMatch()) examples << file;
        }
        examples.sort();
        // Every one with a diagram other than a plot, and every third with
        // plots alone (all of them: VIEW_ALL_EXAMPLES=1).
        for (int i = 0; i < examples.size(); ++i)
            if (i % 3 == 0 || qEnvironmentVariableIsSet("VIEW_ALL_EXAMPLES")) others << examples[i];
        others.sort();
        for (const QString& file : std::as_const(others))
            QTest::newRow(qPrintable(QDir(QStringLiteral(QUCS_EXAMPLES_DIR)).relativeFilePath(file))) << file;
    }

    void viewAllShowsEveryExampleWhole()
    {
        QFETCH(QString, file);
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, file);
        // (One whose Verilog-A model asks to be compiled as it opens is
        // answered no, and does not open.)
        if (s == nullptr) QSKIP("it does not open here");
        s->showAll();
        pump();
        const QImage canvas = canvasImage(s);
        const QColor paper = s->viewport()->palette().color(s->viewport()->backgroundRole());
        const QRect ink = inkOf(canvas, paper);
        QVERIFY(!ink.isEmpty());
        const QString what = QStringLiteral("ink (%1,%2 %3x%4) in a canvas of %5x%6 at scale %7")
                                 .arg(ink.x()).arg(ink.y()).arg(ink.width()).arg(ink.height())
                                 .arg(canvas.width()).arg(canvas.height()).arg(s->getScale());
        QVERIFY2(canvas.rect().adjusted(2, 2, -2, -2).contains(ink), qPrintable(what));
        // Zoomed out so far that the canvas leaves texts out (they would be
        // under four pixels tall - with a smaller font, sooner), the ink is
        // not all the schematic: a long text (a .model line) has no ink
        // then. Only that nothing is cut off is told then.
        const bool textsDrawn = QFontMetrics(s->viewport()->font()).lineSpacing() * s->getScale() >= qucs_s::lod::kSmallestLine;
        if (textsDrawn)
            QVERIFY2(ink.width() > canvas.width() * 3 / 4 || ink.height() > canvas.height() * 3 / 4, qPrintable(what));
        s->setChanged(false);
    }
};

QTEST_MAIN(TestViewAll)
#include "test_view_all.moc"
