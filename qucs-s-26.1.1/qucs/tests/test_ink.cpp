/*
 * The colours of a schematic on its paper (ink.h): on light paper as they
 * always were, on dark paper lightened where they would not show - every
 * colour the built-in symbols use, the canvas as drawn, diagrams as light
 * cards, prints unchanged - and the setting that makes the paper follow
 * the dark theme.
 */
#include <QtTest>
#include <QImage>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QLinearGradient>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "wire.h"
#include "ink.h"
#include "apptheme.h"
#include "diagrams/diagram.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s;

namespace {

const QColor kDark = ink::darkPaperColour();
const QColor kCream(255, 250, 225);

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The palette's components, instantiated.
QList<Component*> paletteComponents()
{
    QList<Component*> out;
    for (const QString& cat : Category::getCategories())
        for (Module* m : Category::getModules(cat)) {
            if (m->info == nullptr) continue;
            QString name;
            char* file = nullptr;
            Element* e = m->info(name, file, true);
            if (auto* c = dynamic_cast<Component*>(e)) out << c;
            else delete e;
        }
    return out;
}

// A horizontal wire of some length, not selected.
Wire* longWire(Schematic& doc)
{
    Wire* best = nullptr;
    for (Wire* w : doc.a_DocWires)
        if (w->y1 == w->y2 && (best == nullptr || std::abs(w->x2 - w->x1) > std::abs(best->x2 - best->x1)))
            best = w;
    return best;
}

} // namespace

class TestInk : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.BGColor = kCream;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void cleanupTestCase()
    {
        apptheme::apply(apptheme::Light);
        QucsSettings.PaperFollowsTheme = false;
    }

    void onLightPaperNothingChanges()
    {
        QCOMPARE(ink::paper(), QColor(Qt::white));   // nothing set: light paper
        QVERIFY(!ink::darkPaper());
        for (const QColor c : {QColor(Qt::darkBlue), QColor(Qt::black), QColor(Qt::yellow), QColor(200, 200, 200)})
            QCOMPARE(ink::on(c), c);
        const ink::Paper cream(kCream);
        QVERIFY(!ink::darkPaper());
        QCOMPARE(ink::on(Qt::darkBlue), QColor(Qt::darkBlue));
    }

    void onDarkPaperWhatWouldVanishIsLightened()
    {
        const ink::Paper dark(kDark);
        QVERIFY(ink::darkPaper());
        // Dark blue: the same hue, light enough to read.
        const QColor blue = ink::on(Qt::darkBlue);
        QCOMPARE(blue.hslHue(), QColor(Qt::darkBlue).hslHue());
        QVERIFY(blue.lightnessF() > 0.5);
        QVERIFY(ink::contrast(blue, kDark) >= 3.0);
        QVERIFY(ink::contrast(QColor(Qt::darkBlue), kDark) < 3.0);
        // Black: a light grey.
        const QColor black = ink::on(Qt::black);
        QCOMPARE(black.hslSaturation(), 0);
        QVERIFY(ink::contrast(black, kDark) >= 3.0);
        // Dark red keeps its hue; what shows already is left alone.
        QCOMPARE(ink::on(Qt::darkRed).hslHue(), 0);
        QVERIFY(ink::contrast(ink::on(Qt::darkRed), kDark) >= 3.0);
        for (const QColor c : {QColor(Qt::red), QColor(Qt::white), QColor(Qt::yellow), QColor(Qt::lightGray)})
            QCOMPARE(ink::on(c), c);
        // Transparency stays.
        QCOMPARE(ink::on(QColor(0, 0, 128, 100)).alpha(), 100);
        QCOMPARE(ink::on(QColor(0, 0, 0, 0)), QColor(0, 0, 0, 0));
        // Pens and brushes: the colour only.
        const QPen pen = ink::on(QPen(Qt::darkBlue, 3, Qt::DashLine));
        QCOMPARE(pen.width(), 3);
        QCOMPARE(pen.style(), Qt::DashLine);
        QCOMPARE(pen.color(), blue);
        QCOMPARE(ink::on(QBrush(Qt::darkBlue)).color(), blue);
        QCOMPARE(ink::on(QBrush(Qt::NoBrush)).style(), Qt::NoBrush);
        QLinearGradient g(0, 0, 1, 1);
        g.setColorAt(0, Qt::black);
        QCOMPARE(ink::on(QBrush(g)), QBrush(g));   // a gradient is someone's design
    }

    void papersNest()
    {
        {
            const ink::Paper dark(kDark);
            QVERIFY(ink::darkPaper());
            {
                const ink::Paper card(Qt::white);
                QVERIFY(!ink::darkPaper());
                QCOMPARE(ink::on(Qt::black), QColor(Qt::black));
            }
            QVERIFY(ink::darkPaper());
        }
        QVERIFY(!ink::darkPaper());
    }

    // Every colour a built-in symbol is drawn with shows on dark paper.
    void everySymbolColourShowsOnDarkPaper()
    {
        const ink::Paper dark(kDark);
        QStringList faint;
        int colours = 0;
        for (Component* c : paletteComponents()) {
            auto check = [&](const QColor& colour, const char* what) {
                if (!colour.isValid() || colour.alpha() == 0) return;
                ++colours;
                if (ink::contrast(ink::on(colour), kDark) < 3.0)
                    faint << QStringLiteral("%1 %2 %3").arg(c->Model, what, colour.name());
            };
            for (auto* l : c->Lines) check(l->penHint().color(), "line");
            for (auto* a : c->Arcs) check(a->penHint().color(), "arc");
            for (auto* r : c->Rects) {
                check(r->penHint().color(), "rect");
                if (r->brushHint().style() != Qt::NoBrush) check(r->brushHint().color(), "fill");
            }
            for (auto* e : c->Ellipses) {
                check(e->penHint().color(), "ellipse");
                if (e->brushHint().style() != Qt::NoBrush) check(e->brushHint().color(), "fill");
            }
            for (auto* p : c->Polylines) check(p->penHint().color(), "polyline");
            for (auto* t : c->Texts) check(t->Color, "text");
            delete c;
        }
        QVERIFY(colours > 1000);
        QVERIFY2(faint.isEmpty(), qPrintable(faint.join("\n")));
    }

    // The canvas on dark paper: the wires come out light; a diagram is a
    // light card; a print of the same schematic is as it always was.
    void theCanvasOnDarkPaper()
    {
        Schematic doc(nullptr, QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/General Electronics/gain_phase_AC.sch"));
        QVERIFY(doc.load());
        misc::setWidgetBackgroundColor(doc.viewport(), kDark);
        doc.resize(1200, 800);
        doc.show();
        doc.showAll();
        QVERIFY(QTest::qWaitForWindowExposed(&doc));
        const QImage canvas = doc.viewport()->grab().toImage();

        Wire* w = longWire(doc);
        QVERIFY(w != nullptr);
        const QPoint middle = doc.modelToViewport(QPoint((w->x1 + w->x2) / 2, w->y1));
        const QColor wire = canvas.pixelColor(middle);
        QVERIFY2(ink::contrast(wire, kDark) >= 3.0, qPrintable(wire.name()));
        QVERIFY(wire.blue() > wire.red());   // still blue

        QVERIFY(!doc.a_DocDiags.empty());
        const QRect d = doc.a_DocDiags.front()->boundingRect();
        const QColor card = canvas.pixelColor(doc.modelToViewport(d.topLeft() + QPoint(3, 3)));
        QCOMPARE(card, QColor(Qt::white));

        // Printed or exported: on white, dark blue as ever.
        const QRect all = doc.allBoundingRect();
        QImage print(all.size() + QSize(2, 2), QImage::Format_ARGB32);
        print.fill(Qt::white);
        QPainter painter(&print);
        painter.translate(-all.topLeft() + QPoint(1, 1));
        doc.paintSchToViewpainter(&painter, true);
        painter.end();
        const QColor printed = print.pixelColor(QPoint((w->x1 + w->x2) / 2, w->y1) - all.topLeft() + QPoint(1, 1));
        QCOMPARE(printed, QColor(Qt::darkBlue));
        QVERIFY(!ink::darkPaper());   // the canvas' paper went with its paint
    }

    // The setting: dark paper in the dark theme, the background colour
    // otherwise; every open schematic follows.
    void thePaperCanFollowTheTheme()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/General Electronics/gain_phase_AC.sch")));
        Schematic* doc = app.currentSchematic();
        auto paperOf = [](Schematic* s) { return s->viewport()->palette().color(s->viewport()->backgroundRole()); };

        apptheme::apply(apptheme::Dark);
        QucsSettings.PaperFollowsTheme = false;
        app.applyPaper();
        QCOMPARE(misc::paperColor(), kCream);   // off: the background colour, whatever the theme
        QCOMPARE(paperOf(doc), kCream);

        QucsSettings.PaperFollowsTheme = true;
        app.applyPaper();
        QCOMPARE(misc::paperColor(), kDark);
        QCOMPARE(paperOf(doc), kDark);

        apptheme::apply(apptheme::Light);
        app.applyPaper();
        QCOMPARE(paperOf(doc), kCream);   // light theme: light paper

        // A dark background colour of one's own is dark paper too.
        QucsSettings.PaperFollowsTheme = false;
        QucsSettings.BGColor = QColor(10, 10, 40);
        app.applyPaper();
        QVERIFY(ink::isDark(paperOf(doc)));
        QucsSettings.BGColor = kCream;
        app.applyPaper();
        app.closeAllFiles();
    }
};

QTEST_MAIN(TestInk)
#include "test_ink.moc"
