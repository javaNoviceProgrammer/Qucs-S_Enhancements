/*
 * How a graph's line is drawn:
 *  - dashed, dotted and long-dashed graphs look dashed, dotted and
 *    long-dashed however close their points are (#1723). The pattern used
 *    to start again at every point, so a graph whose points are closer
 *    than a dash - about any simulation - was drawn solid;
 *  - a stroke of two points is drawn: a graph of two samples, the first
 *    curve of a sweep with two samples a step, the last segment back inside
 *    a diagram with a fixed range. Diagram::calcData() erased all of them.
 */
#include <QtTest>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <functional>

#include "config.h"
#include "main.h"
#include "module.h"
#include "misc.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/graph.h"

namespace {

// 600 x 300, x autoscaled over 0..1, y fixed from 0 to 1: a graph above 1
// is clipped at the top edge.
const char* kRect =
    "<Rect 0 300 600 300 3 #c0c0c0 1 00 1 0 0.2 1 0 0 0.5 1 1 -0.1 0.5 1.1 315 0 225 1 0 0 \"\" \"\" \"\">";

// A dataset: x from 0 to 1 in `points` steps, then per step of `levels`
// (if any) the values of `v` - value(level index, point index).
QString writeDataset(const QString& path, int points, const QList<double>& levels,
                     const std::function<double(int, int)>& value)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QTextStream out(&f);
    out << "<Qucs Dataset " PACKAGE_VERSION ">\n<indep x " << points << ">\n";
    for (int i = 0; i < points; ++i) out << "  " << double(i) / (points - 1) << "\n";
    out << "</indep>\n";
    if (!levels.isEmpty()) {
        out << "<indep level " << levels.size() << ">\n";
        for (double l : levels) out << "  " << l << "\n";
        out << "</indep>\n<dep v x level>\n";
    } else {
        out << "<dep v x>\n";
    }
    for (int s = 0; s < std::max<int>(1, levels.size()); ++s)
        for (int i = 0; i < points; ++i) out << "  " << value(s, i) << "\n";
    out << "</dep>\n";
    return path;
}

// The diagram with the one graph `v`, blue, in `style` and `thick`ness.
RectDiagram* makeDiagram(int style, int thick, const QString& dataset)
{
    QString body = QStringLiteral("<\"v\" #0000ff %1 3 0 %2 0>\n</Rect>\n").arg(thick).arg(style);
    QTextStream stream(&body, QIODevice::ReadOnly);
    auto* d = new RectDiagram();
    if (!d->load(kRect, &stream)) { delete d; return nullptr; }
    d->loadGraphData(dataset);
    return d;
}

// Paints the diagram (box at [0,x2] x [0,y2] of the image).
QImage render(Diagram* d)
{
    QImage img(d->x2 + 1, d->y2 + 1, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    d->paintDiagram(&p);
    return img;
}

bool isBlue(const QColor& c) { return c.blue() > 200 && c.red() < 120 && c.green() < 120; }

// The row of the image a value from 0 to 1 is drawn on (y up from the bottom).
int rowOf(const Diagram* d, double value) { return d->y2 - int(value * d->y2 + 0.5); }

// The runs of blue along a row (give or take a pixel), inside the box.
int runsAlong(const QImage& img, int row)
{
    int runs = 0;
    bool inside = false;
    for (int x = 5; x < img.width() - 6; ++x) {
        bool blue = false;
        for (int dy = -1; dy <= 1; ++dy) blue = blue || isBlue(img.pixelColor(x, row + dy));
        if (blue && !inside) ++runs;
        inside = blue;
    }
    return runs;
}

// Blue pixels in a rectangle of the image.
int blueIn(const QImage& img, const QRect& area)
{
    int n = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (isBlue(img.pixelColor(x, y))) ++n;
    return n;
}

} // namespace

class TestGraphStyle : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void aPatternRunsOnFromPointToPoint_data()
    {
        QTest::addColumn<int>("points");
        QTest::addColumn<int>("style");
        QTest::addColumn<int>("thick");
        QTest::addColumn<int>("fewest");
        QTest::addColumn<int>("most");

        // 600 pixels: a solid line is one run, a pattern of on + off times
        // the pen's width about 600 / ((on + off) * width) runs (dash
        // 10 + 6, dot 2 + 4, long dash 24 + 8), however many points there
        // are - 400 is 1.5 pixels a point, 5000 less than one, and 2 a
        // single line.
        for (int points : {2, 400, 5000}) {
            const QByteArray n = QByteArray::number(points);
            QTest::newRow("solid, " + n) << points << int(GRAPHSTYLE_SOLID) << 2 << 1 << 1;
            QTest::newRow("dash, " + n) << points << int(GRAPHSTYLE_DASH) << 2 << 15 << 22;
            QTest::newRow("dot, " + n) << points << int(GRAPHSTYLE_DOT) << 2 << 40 << 60;
            QTest::newRow("long dash, " + n) << points << int(GRAPHSTYLE_LONGDASH) << 2 << 7 << 11;
            QTest::newRow("thin dash, " + n) << points << int(GRAPHSTYLE_DASH) << 1 << 30 << 44;
        }
    }

    void aPatternRunsOnFromPointToPoint()
    {
        QFETCH(int, points);
        QFETCH(int, style);
        QFETCH(int, thick);
        QFETCH(int, fewest);
        QFETCH(int, most);

        const QString data = writeDataset(dir.filePath(QStringLiteral("flat%1.dat").arg(points)), points, {},
                                          [](int, int) { return 0.5; });
        QVERIFY(!data.isEmpty());
        QScopedPointer<RectDiagram> d(makeDiagram(style, thick, data));
        QVERIFY(d);
        QCOMPARE(d->Graphs.first()->countY, 1);

        const int runs = runsAlong(render(d.data()), rowOf(d.data(), 0.5));
        QVERIFY2(runs >= fewest && runs <= most,
                 qPrintable(QStringLiteral("%1 runs, expected %2..%3").arg(runs).arg(fewest).arg(most)));
    }

    // A parameter sweep is one stroke per value: each starts its own
    // pattern and none is joined to the next.
    void eachCurveOfASweepIsItsOwn_data()
    {
        QTest::addColumn<int>("points");
        QTest::addColumn<int>("style");
        QTest::addColumn<int>("fewest");
        QTest::addColumn<int>("most");
        QTest::newRow("dashed") << 400 << int(GRAPHSTYLE_DASH) << 15 << 22;
        // Two samples a step: the first curve was erased.
        QTest::newRow("two samples") << 2 << int(GRAPHSTYLE_SOLID) << 1 << 1;
        QTest::newRow("two samples, dashed") << 2 << int(GRAPHSTYLE_DASH) << 15 << 22;
    }

    void eachCurveOfASweepIsItsOwn()
    {
        QFETCH(int, points);
        QFETCH(int, style);
        QFETCH(int, fewest);
        QFETCH(int, most);

        const QString data = writeDataset(dir.filePath(QStringLiteral("sweep%1.dat").arg(points)), points,
                                          {0.25, 0.75}, [](int s, int) { return s ? 0.75 : 0.25; });
        QScopedPointer<RectDiagram> d(makeDiagram(style, 2, data));
        QVERIFY(d);
        QCOMPARE(d->Graphs.first()->countY, 2);
        const QImage img = render(d.data());

        for (double level : {0.25, 0.75}) {
            const int runs = runsAlong(img, rowOf(d.data(), level));
            QVERIFY2(runs >= fewest && runs <= most,
                     qPrintable(QStringLiteral("level %1: %2 runs, expected %3..%4").arg(level).arg(runs).arg(fewest).arg(most)));
        }
        // Between the two levels: a line from the end of the first curve
        // back to the start of the second would cross it.
        QCOMPARE(blueIn(img, QRect(5, rowOf(d.data(), 0.75) + 15, d->x2 - 10,
                                   rowOf(d.data(), 0.25) - rowOf(d.data(), 0.75) - 30)), 0);
    }

    // Above the fixed range until the last point: the segment from the top
    // edge down to it is all that is inside, and it is drawn.
    void theLastSegmentBackInsideIsDrawn()
    {
        const QString data = writeDataset(dir.filePath("back.dat"), 5, {},
                                          [](int, int i) { return i < 4 ? 2.0 : 0.5; });
        QScopedPointer<RectDiagram> d(makeDiagram(GRAPHSTYLE_SOLID, 2, data));
        QVERIFY(d);
        const QImage img = render(d.data());
        // From (0.92, 1) to (1, 0.5): the right twelfth, the upper half.
        QVERIFY2(blueIn(img, QRect(d->x2 * 11 / 12 - 5, 3, d->x2 / 12, d->y2 / 2 - 3)) > 50,
                 qPrintable(QString::number(blueIn(img, img.rect()))));
        // Nothing of it further left: the part above 1 is clipped.
        QCOMPARE(blueIn(img, QRect(5, 3, d->x2 * 3 / 4, d->y2 - 6)), 0);
    }
};

QTEST_MAIN(TestGraphStyle)
#include "test_graph_style.moc"
