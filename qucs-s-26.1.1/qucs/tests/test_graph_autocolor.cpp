/*
 * Auto colors: a graph whose curves - the values of a swept parameter -
 * are drawn each in a color of its own, from a fixed palette in its order,
 * the auto graphs of a diagram sharing it; past the palette the colors
 * come round again with the next line style. Saved as an eighth field of
 * the graph that older versions do not read. The legend names every
 * curve by its values; the diagram dialog has the option.
 */
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QImage>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "diagrams/diagramdialog.h"
#include "diagrams/graph.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/tabdiagram.h"

namespace {

// A rectangular diagram with the graphs of \a body, from a save line; the
// y axis from \a ylo to \a yhi when given, else scaled to the data.
RectDiagram* makeDiagram(const QString& body, double ylo = 0, double yhi = 0, int legend = 0)
{
    const QString yAxis = ylo < yhi ? QStringLiteral("0 %1 %2 %3").arg(ylo).arg((yhi - ylo) / 5).arg(yhi)
                                    : QStringLiteral("1 0 1 1");
    const QString line = QStringLiteral("<Rect 0 400 600 400 3 #c0c0c0 1 00 1 0 1 1 %1 1 -1 0.5 1 315 0 225 1 0 0 %2 -1 "
                                        "\"\" \"\" \"\">")
                             .arg(yAxis)
                             .arg(legend);
    QString text = body + "</Rect>\n";
    QTextStream stream(&text, QIODevice::ReadOnly);
    auto* d = new RectDiagram();
    if (!d->load(line, &stream)) {
        delete d;
        return nullptr;
    }
    return d;
}

// A dataset: v over x (3 points) for \a curves values of r1 (1k, 2k, ...),
// curve k at the height k + 1; w over x for the values of c1 (2 of them).
bool writeDataset(const QString& file, int curves)
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QTextStream s(&f);
    s << "<Qucs Dataset " PACKAGE_VERSION ">\n<indep x 3>\n0\n0.5\n1\n</indep>\n";
    s << "<indep r1 " << curves << ">\n";
    for (int k = 0; k < curves; ++k) s << 1000.0 * (k + 1) << "\n";
    s << "</indep>\n<dep v x r1>\n";
    for (int k = 0; k < curves; ++k)
        for (int i = 0; i < 3; ++i) s << k + 1 << "\n";
    s << "</dep>\n<indep c1 2>\n1e-07\n2e-07\n</indep>\n<dep w x c1>\n";
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < 3; ++i) s << 20 + k << "\n";
    s << "</dep>\n";
    return true;
}

QImage render(Diagram* d)
{
    QImage img(d->x2 + 1, d->y2 + 1, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setFont(QFont("Helvetica", 12));
    d->paintDiagram(&p);
    return img;
}

int countColor(const QImage& img, const QRect& area, QRgb color)
{
    int n = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (img.pixel(x, y) == color) ++n;
    return n;
}

} // namespace

class TestGraphAutoColor : public QObject
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

    // An eighth field, 1 when on; a graph without it saves as before,
    // and files without the field (or with junk in it) load without.
    void savedAsAnEighthField()
    {
        Graph g(nullptr);
        QVERIFY(g.load("<\"ngspice/ngsweep1.v(out)\" #0000ff 2 3 0 0 0 1>"));
        QVERIFY(g.autoColor);
        QCOMPARE(g.save(), QString("\t<\"ngspice/ngsweep1.v(out)\" #0000ff 2 3 0 0 0 1>"));
        QScopedPointer<Graph> copy(g.sameNewOne());
        QVERIFY(copy->autoColor);

        Graph old(nullptr);
        QVERIFY(old.load("<\"v\" #ff0000 1 3 0 1 1>"));
        QVERIFY(!old.autoColor);
        QCOMPARE(old.save(), QString("\t<\"v\" #ff0000 1 3 0 1 1>"));
        Graph junk(nullptr);
        QVERIFY(junk.load("<\"v\" #ff0000 1 3 0 1 1 x>"));
        QVERIFY(!junk.autoColor);
        Graph off(nullptr);
        QVERIFY(off.load("<\"v\" #ff0000 1 3 0 1 1 0>"));
        QVERIFY(!off.autoColor);
        QCOMPARE(off.save(), QString("\t<\"v\" #ff0000 1 3 0 1 1>"));
    }

    // The palette in its order, its colors apart; the diagrams it applies to.
    void thePalette()
    {
        const QList<QColor>& p = Graph::autoPalette();
        QCOMPARE(p.size(), 8);
        QCOMPARE(p.first(), QColor("#2a78d6"));
        QCOMPARE(p.at(1), QColor("#eb6834"));
        QCOMPARE(p.last(), QColor("#e34948"));
        for (int i = 0; i < p.size(); ++i)
            for (int j = i + 1; j < p.size(); ++j) QVERIFY(p.at(i) != p.at(j));
        for (const char* name : {"Rect", "Polar", "Smith", "ySmith", "PS", "SP", "Curve"})
            QVERIFY(Graph::autoColorApplies(name));
        for (const char* name : {"Tab", "Truth", "Time", "Rect3D", "Histogram"}) QVERIFY(!Graph::autoColorApplies(name));
    }

    // Each curve its color, the palette's order, the next line style when
    // the colors come round; the graphs of a diagram go on from each
    // other; a graph not in auto keeps its color.
    void eachCurveItsColor()
    {
        const QString data = dir.filePath("family.dat");
        QVERIFY(writeDataset(data, 10));
        QScopedPointer<RectDiagram> d(makeDiagram("<\"v\" #0000ff 2 3 0 0 0 1>\n<\"w\" #ff0000 2 3 0 1 0 1>\n"
                                                  "<\"w\" #00ff00 2 3 0 0 0>\n"));
        QVERIFY(d);
        d->loadGraphData(data);
        Graph* v = d->Graphs.at(0);
        Graph* w = d->Graphs.at(1);
        Graph* plain = d->Graphs.at(2);
        QCOMPARE(v->countY, 10);
        QCOMPARE(w->countY, 2);
        const QList<QColor>& p = Graph::autoPalette();
        for (int k = 0; k < 8; ++k) {
            QCOMPARE(v->curveColor(k), p.at(k));
            QCOMPARE(v->curveStyle(k), GRAPHSTYLE_SOLID);
        }
        QCOMPARE(v->curveColor(8), p.at(0));
        QCOMPARE(v->curveStyle(8), GRAPHSTYLE_DASH);
        QCOMPARE(v->curveColor(9), p.at(1));
        // The second auto graph: after the first's ten curves, dashed from a
        // dash: dotted.
        QCOMPARE(w->curveColor(0), p.at(10 % 8));
        QCOMPARE(w->curveStyle(0), GRAPHSTYLE_DOT);
        QCOMPARE(plain->curveColor(0), QColor("#00ff00"));
        QCOMPARE(plain->curveStyle(3), GRAPHSTYLE_SOLID);
        QVERIFY(!plain->colorsEachCurve());

        QCOMPARE(v->curveLabel(0), QString("r1=1k"));
        QCOMPARE(v->curveLabel(9), QString("r1=10k"));
        QCOMPARE(w->curveLabel(1), QString("c1=200n"));

        // Symbols keep their shape through the rounds.
        w->Style = GRAPHSTYLE_STAR;
        QCOMPARE(w->curveStyle(0), GRAPHSTYLE_STAR);

        // Only where curves are drawn.
        TabDiagram tab;
        Graph t(&tab);
        t.autoColor = true;
        t.Color = Qt::red;
        QVERIFY(!t.colorsEachCurve());
        QCOMPARE(t.curveColor(3), QColor(Qt::red));
    }

    // Drawn so: the eight colors on the diagram and not the graph's own;
    // off, only the graph's own.
    void drawnInThem()
    {
        const QString data = dir.filePath("drawn.dat");
        QVERIFY(writeDataset(data, 8));
        QScopedPointer<RectDiagram> d(makeDiagram("<\"v\" #0000ff 2 3 0 0 0 1>\n"));
        QVERIFY(d);
        d->loadGraphData(data);
        QImage img = render(d.data());
        for (const QColor& c : Graph::autoPalette())
            QVERIFY2(countColor(img, img.rect(), c.rgb()) > 20, qPrintable(c.name()));
        QCOMPARE(countColor(img, img.rect(), qRgb(0, 0, 255)), 0);
        // Selected: one highlight for the whole graph, as before.
        d->Graphs.first()->isSelected = true;
        img = render(d.data());
        QCOMPARE(countColor(img, img.rect(), Graph::autoPalette().at(3).rgb()), 0);
        d->Graphs.first()->isSelected = false;

        d->Graphs.first()->autoColor = false;
        img = render(d.data());
        QVERIFY(countColor(img, img.rect(), qRgb(0, 0, 255)) > 100);
        for (const QColor& c : Graph::autoPalette()) QCOMPARE(countColor(img, img.rect(), c.rgb()), 0);

        // Dashed: the dashes in the curves' colors too.
        d->Graphs.first()->autoColor = true;
        d->Graphs.first()->Style = GRAPHSTYLE_DASH;
        d->loadGraphData(data);
        img = render(d.data());
        for (const QColor& c : Graph::autoPalette()) QVERIFY2(countColor(img, img.rect(), c.rgb()) > 10, qPrintable(c.name()));
    }

    // The legend: a row for every curve, in its color; the graph's name on
    // the axis in plain ink.
    void theLegendNamesEveryCurve()
    {
        const QString data = dir.filePath("legend.dat");
        QVERIFY(writeDataset(data, 4));
        // The curves below the y axis: only the legend shows their colors.
        QScopedPointer<RectDiagram> d(makeDiagram("<\"v\" #0000ff 2 3 0 0 0 1>\n", 100, 200, Diagram::LegendTopRight));
        QVERIFY(d);
        d->loadGraphData(data);
        QImage img = render(d.data());
        const QRect corner(d->x2 / 2, 0, d->x2 / 2, d->y2 / 2);
        for (int k = 0; k < 4; ++k)
            QVERIFY2(countColor(img, corner, Graph::autoPalette().at(k).rgb()) > 5, qPrintable(QString::number(k)));
        QCOMPARE(countColor(img, img.rect(), Graph::autoPalette().at(4).rgb()), 0);
        // A row a curve, in their order: each sample line below the last.
        auto rowOf = [&](const QColor& c) {
            double sum = 0;
            int n = 0;
            for (int y = corner.top(); y <= corner.bottom(); ++y)
                for (int x = corner.left(); x <= corner.right(); ++x)
                    if (img.pixel(x, y) == c.rgb()) {
                        sum += y;
                        ++n;
                    }
            return n > 0 ? sum / n : -1.0;
        };
        const QFontMetricsF fm(QFont("Helvetica", 12));
        for (int k = 1; k < 4; ++k) {
            const double step = rowOf(Graph::autoPalette().at(k)) - rowOf(Graph::autoPalette().at(k - 1));
            QVERIFY2(step > 0.5 * fm.height() && step < 1.5 * fm.height(), qPrintable(QString::number(step)));
        }
        // Not in the graph's own blue: the name on the axis is plain ink.
        QCOMPARE(countColor(img, img.rect(), qRgb(0, 0, 255)), 0);
    }

    // The diagram dialog: "auto" by the color; on, the color button is off
    // and the legend comes on; applied to the diagram. Not for tables.
    void theDialogHasTheOption()
    {
        QScopedPointer<RectDiagram> d(makeDiagram("<\"v\" #0000ff 2 3 0 0 0>\n"));
        QVERIFY(d);
        auto* dialog = new DiagramDialog(d.data(), nullptr);   // WA_DeleteOnClose
        QCheckBox* automatic = nullptr;
        for (QCheckBox* box : dialog->findChildren<QCheckBox*>())
            if (box->text() == "auto") automatic = box;
        QVERIFY(automatic != nullptr);
        QVERIFY(!automatic->isEnabled());   // no graph chosen yet
        auto* list = dialog->findChild<QTableWidget*>(QString(), Qt::FindChildrenRecursively);
        QTableWidget* graphs = nullptr;
        for (QTableWidget* t : dialog->findChildren<QTableWidget*>())
            if (t->columnCount() == 5) graphs = t;
        QVERIFY(graphs != nullptr && list != nullptr);
        graphs->selectRow(0);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotSelectGraph", Q_ARG(QTableWidgetItem*, graphs->item(0, 0))));
        QVERIFY(automatic->isEnabled());
        QVERIFY(!automatic->isChecked());
        automatic->setChecked(true);
        QCOMPARE(graphs->item(0, 1)->text(), QString("auto"));
        QComboBox* legend = nullptr;
        for (QComboBox* box : dialog->findChildren<QComboBox*>())
            if (box->findText("top right") >= 0) legend = box;
        QVERIFY(legend != nullptr);
        QCOMPARE(legend->currentIndex(), int(Diagram::LegendTopRight));
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(d->Graphs.first()->autoColor);
        QCOMPARE(d->legendPos, int(Diagram::LegendTopRight));

        automatic->setChecked(false);
        QCOMPARE(graphs->item(0, 1)->text(), QString());
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(!d->Graphs.first()->autoColor);
        dialog->close();

        TabDiagram tab;
        auto* tabDialog = new DiagramDialog(&tab, nullptr);
        for (QCheckBox* box : tabDialog->findChildren<QCheckBox*>()) QVERIFY(box->text() != "auto");
        tabDialog->close();
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestGraphAutoColor test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_graph_autocolor.moc"
