// The 3D diagram (Rect3DDiagram): its grid on the box's back walls, its
// hidden lines with surfaces of different sizes, its axes' numbers.
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QFontMetricsF>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>
#include <functional>
#include <memory>

#include "config.h"
#include "main.h"
#include "module.h"
#include "settings.h"
#include "misc.h"
#include "diagrams/diagramdialog.h"
#include "diagrams/diagramtheme.h"
#include "diagrams/graph.h"
#include "diagrams/rect3ddiagram.h"
#include "isolated_settings.h"

using Part = qucs_s::diagramtheme::Part;

namespace {

// A column of values of a dataset's block.
void block(QTextStream& s, const QString& header, int n, const std::function<double(int)>& value)
{
    s << "<" << header << ">\n";
    for (int i = 0; i < n; ++i) s << QString::number(value(i), 'g', 17) << "\n";
    s << "</" << header.section(' ', 0, 0) << ">\n";
}

// A bowl over a 7 x 5 die (71 x 51 points, 0 at its centre, -18 at its
// corners), as a review plotted it; and two flat patches
// side by side at height 0, apart (x below -0.5 and above 0.5), of 15 x 11
// and 41 x 31 points spaced a little unevenly - no two of their cells as far
// from the viewer.
QString writeDataset(const QString& dir)
{
    const QString file = dir + "/rect3d.dat";
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return {};
    QTextStream s(&f);
    s << "<Qucs Dataset " PACKAGE_VERSION ">\n";
    const auto x = [](int i) { return -3.5 + 0.1 * i; };
    const auto y = [](int j) { return -2.5 + 0.1 * j; };
    block(s, "indep x 71", 71, x);
    block(s, "indep y 51", 51, y);
    block(s, "dep w x y", 71 * 51, [&](int k) {
        const double u = x(k % 71) / 3.5, v = y(k / 71) / 2.5;
        return -9.0 * (u * u + v * v);
    });
    const auto uneven = [](double from, double to, int n) {
        return [=](int i) { const double t = double(i) / (n - 1); return from + (to - from) * (t + 0.03 * t * (1 - t) * (1 + 0.37 * i)); };
    };
    block(s, "indep lx 15", 15, uneven(-3.5, -0.5, 15));
    block(s, "indep ly 11", 11, uneven(-2.5, 2.5, 11));
    block(s, "dep lid lx ly", 15 * 11, [](int) { return 0.0; });
    block(s, "indep px 41", 41, uneven(0.5, 3.5, 41));
    block(s, "indep py 31", 31, uneven(-2.5, 2.5, 31));
    block(s, "dep patch px py", 41 * 31, [](int) { return 0.0; });
    return file;
}

// A 3D diagram of \a w x \a h, \a flags its grid (1) and its hidden lines
// (2), its axes automatic or \a limits, over \a traces.
std::unique_ptr<Rect3DDiagram> diagram(const QString& data, const QStringList& traces, int flags, int w = 440, int h = 360,
                                       const QString& limits = QStringLiteral("1 0 1 1 1 0 1 1 1 0 1 1"))
{
    const QString line = QStringLiteral("<Rect3D 60 460 %1 %2 %3 #c0c0c0 1 00 %4 315 0 225 1 0 0 \"\" \"\" \"\">")
                             .arg(w).arg(h).arg(flags).arg(limits);
    QString body;
    // (On the z axis, its last field 1, as the dialog puts a 3D diagram's.)
    for (const QString& t : traces) body += QStringLiteral("<\"%1\" #0000ff 0 3 0 0 1>\n").arg(t);
    body += QStringLiteral("</Rect3D>\n");
    QTextStream stream(&body, QIODevice::ReadOnly);
    auto d = std::make_unique<Rect3DDiagram>();
    if (!d->load(line, &stream)) return nullptr;
    d->loadGraphData(data);
    return d;
}

QList<qucs::Line*> gridLines(const Diagram& d)
{
    QList<qucs::Line*> grid;
    for (qucs::Line* l : d.Lines)
        if (int(l->part) == int(Part::Grid)) grid << l;
    return grid;
}

double lengthOf(const QList<qucs::Line*>& lines)
{
    double sum = 0;
    for (const qucs::Line* l : lines) sum += std::hypot(double(l->x2 - l->x1), double(l->y2 - l->y1));
    return sum;
}

// Where \a text is drawn, as Diagram::textRect() has it: from its
// baseline, turned.
QRectF textRect(const Text& text, const QFontMetricsF& metrics)
{
    const QRectF box(0, -metrics.ascent(), metrics.horizontalAdvance(text.s), metrics.height());
    return QTransform().translate(text.x, -text.y).rotate(text.angle()).mapRect(box);
}

// What a graph draws: its points and its ends, in order.
QList<QList<double>> drawn(const Graph& g)
{
    QList<QList<double>> points;
    for (auto p = g.begin(); p != g.end(); ++p) {
        if (p->isGraphEnd()) break;
        points << QList<double>{double(p->isPt()), double(p->isStrokeEnd()), double(p->isBranchEnd()),
                                p->isPt() ? double(p->getScrX()) : 0.0, p->isPt() ? double(p->getScrY()) : 0.0};
    }
    return points;
}

} // namespace

class TestRect3DDiagram : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    QString data;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.font = QFont("Helvetica", 12);   // (what the axes are laid out in)
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        data = writeDataset(dir.path());
        QVERIFY(!data.isEmpty());
    }

    // Its grid on (bit 1 of the flags, as a flat diagram's): lines on the
    // box's three back walls at the axes' numbers, behind its frame (listed
    // first), in the grid's pen; none when it is off. Hidden behind the
    // surface where the hidden lines are, as the axes are; whole where not.
    void theGridIsDrawnWhenItIsOn()
    {
        const auto off = diagram(data, {"w"}, 0);
        QVERIFY(off);
        QVERIFY(gridLines(*off).isEmpty());

        const auto whole = diagram(data, {"w"}, 1);
        QVERIFY(whole);
        const QList<qucs::Line*> lines = gridLines(*whole);
        QVERIFY2(lines.size() >= 12, qPrintable(QString::number(lines.size())));   // (a few numbers on each axis, each on two walls)
        for (int i = 0; i < lines.size(); ++i) QCOMPARE(int(whole->Lines.at(i)->part), int(Part::Grid));
        for (const qucs::Line* l : lines) {
            QCOMPARE(l->style, whole->GridPen);
            for (const QPointF p : {QPointF(l->x1, l->y1), QPointF(l->x2, l->y2)})
                QVERIFY2(p.x() >= -1 && p.x() <= whole->x2 + 1 && p.y() >= -1 && p.y() <= whole->y2 + 1,
                         qPrintable(QStringLiteral("%1, %2").arg(p.x()).arg(p.y())));
        }

        const auto hidden = diagram(data, {"w"}, 3);
        QVERIFY(hidden);
        const QList<qucs::Line*> shown = gridLines(*hidden);
        QVERIFY(!shown.isEmpty());
        QVERIFY2(lengthOf(shown) < lengthOf(lines) - 1, qPrintable(QStringLiteral("%1 of %2").arg(lengthOf(shown)).arg(lengthOf(lines))));
    }

    void itsThemeHasAGrid()
    {
        const Rect3DDiagram d;
        QVERIFY(d.themeParts().contains(Part::Grid));
        QVERIFY(!d.themeParts().contains(Part::PlotArea));   // (a box in space)
    }

    // Its dialog offers the grid, as a flat diagram's does.
    void itsDialogTurnsTheGridOnAndOff()
    {
        const auto d = diagram(data, {"w"}, 3);
        QVERIFY(d);
        auto* dialog = new DiagramDialog(d.get(), nullptr);   // WA_DeleteOnClose
        QCheckBox* grid = nullptr;
        for (QCheckBox* box : dialog->findChildren<QCheckBox*>())
            if (box->text() == QStringLiteral("show Grid")) grid = box;
        QVERIFY(grid != nullptr);
        QVERIFY(grid->isChecked());
        grid->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(!d->xAxis.GridOn);
        QVERIFY(gridLines(*d).isEmpty());
        dialog->close();
    }

    // Two surfaces of different sizes, side by side (neither in front of
    // the other), each drawn as it is alone - whichever is listed first:
    // the cells of both, nearest first, were each taken with the sizes of
    // the graph being gone through, the other's cells walked with the
    // wrong rows (the review's reference floor in pieces).
    void surfacesOfDifferentSizesHideAsAlone()
    {
        const QString limits = QStringLiteral("0 -3.5 1 3.5 0 -2.5 1 2.5 0 -1 0.5 1");
        const auto lid = diagram(data, {"lid"}, 2, 440, 360, limits);
        const auto patch = diagram(data, {"patch"}, 2, 440, 360, limits);
        QVERIFY(lid && patch);
        for (const QStringList& order : {QStringList{"patch", "lid"}, QStringList{"lid", "patch"}}) {
            const auto both = diagram(data, order, 2, 440, 360, limits);
            QVERIFY(both);
            const int l = order.indexOf("lid"), p = order.indexOf("patch");
            QVERIFY2(drawn(*both->Graphs.at(l)) == drawn(*lid->Graphs.first()), qPrintable(order.join(", ")));
            QVERIFY2(drawn(*both->Graphs.at(p)) == drawn(*patch->Graphs.first()), qPrintable(order.join(", ")));
        }
    }

    // No two of its axes' numbers on top of each other - where two axes
    // meet at a corner of the box (the z axis's lowest and the x axis's
    // last, on the review's die): the later one left out, its tick kept.
    void itsNumbersDoNotOverlap()
    {
        const QFontMetricsF metrics(QucsSettings.font);
        for (const QSize size : {QSize(440, 360), QSize(300, 240), QSize(600, 400), QSize(240, 400)}) {
            const auto d = diagram(data, {"w"}, 3, size.width(), size.height());
            QVERIFY(d);
            QList<QPair<QString, QRectF>> numbers;
            for (const Text* t : d->Texts) {
                bool number = false;
                t->s.toDouble(&number);
                if (number) numbers << qMakePair(t->s, textRect(*t, metrics));
            }
            QVERIFY(numbers.size() >= 6);
            for (int i = 0; i < numbers.size(); ++i)
                for (int j = i + 1; j < numbers.size(); ++j)
                    QVERIFY2(!numbers.at(i).second.adjusted(1, 1, -1, -1).intersects(numbers.at(j).second.adjusted(1, 1, -1, -1)),
                             qPrintable(QStringLiteral("%1 and %2 at %3 x %4").arg(numbers.at(i).first, numbers.at(j).first)
                                            .arg(size.width()).arg(size.height())));
        }
    }
};

QTEST_MAIN(TestRect3DDiagram)

#include "test_rect3d_diagram.moc"
