/*
 * A diagram's marker writes its numbers in a notation of its own, or in
 * its diagram's (the default, as before): every number in its box - the
 * position, a complex value as real/imaginary or magnitude/angle, a dB
 * value, a Smith chart's impedance. Its dialog chooses it; it is saved
 * with the marker (older files, and older versions, read the line as
 * before) and copied with it.
 */
#include <QtTest>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "numberformat.h"
#include "diagrams/graph.h"
#include "diagrams/marker.h"
#include "diagrams/markerdialog.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/smithdiagram.h"

using qucs_s::numberformat::Notation;

namespace {

// 600 x 300, the axes automatic, engineering notation (the 1 after 315 0 225).
const char* kRect =
    "<Rect 0 300 600 300 3 #c0c0c0 1 00 1 0 0.2 1 1 0 0.5 1 1 -0.1 0.5 1.1 315 0 225 1 0 0 \"\" \"\" \"\">";
const char* kSmith = "<Smith 0 300 300 300 3 #c0c0c0 1 00 1 0 1 1 1 0 4 1 1 0 1 1 315 0 225 1 0 0 \"\" \"\" \"\">";

// A dataset of \a var over x = 1000, 2000, 3000, its values \a values
// (re, im) - written as a simulator writes them, 1.5e+03-j2.5e-05.
QString writeDataset(const QString& path, const QString& var, const QList<QPair<double, double>>& values)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QTextStream out(&f);
    out << "<Qucs Dataset " PACKAGE_VERSION ">\n<indep x 3>\n  1000\n  2000\n  3000\n</indep>\n<dep " << var << " x>\n";
    for (const auto& [re, im] : values)
        out << "  " << QString::number(re, 'e', 12) << (im < 0 ? "-j" : "+j") << QString::number(std::fabs(im), 'e', 12) << "\n";
    out << "</dep>\n";
    return path;
}

// A diagram of \a head's kind with the graph \a var and the marker line
// \a marker, its data read from \a dataset.
template <typename D>
D* makeDiagram(const char* head, const QString& var, const QString& marker, const QString& dataset)
{
    QString body = QStringLiteral("<\"%1\" #0000ff 2 3 0 0 0>\n  %2\n</%3>\n").arg(var, marker, QString(head).mid(1).section(' ', 0, 0));
    QTextStream stream(&body, QIODevice::ReadOnly);
    auto* d = new D();
    if (!d->load(head, &stream)) {
        delete d;
        return nullptr;
    }
    d->loadGraphData(dataset);
    return d;
}

Marker* markerOf(Diagram* d)
{
    for (Graph* g : d->Graphs)
        if (!g->Markers.isEmpty()) return g->Markers.first();
    return nullptr;
}

} // namespace

class TestMarkerNotation : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    QString complex;   // v over x: 1500-j2.5e-05 at x = 2000

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        complex = writeDataset(dir.filePath("complex.dat"), "v", {{1, 0}, {1500, -2.5e-5}, {3, 0}});
    }

    // Each notation reaches every number: the position and both parts of
    // a complex value (or its magnitude and angle). Automatic writes what
    // a marker always wrote (misc::complexRect's significant digits); the
    // diagram's notation is the default.
    void everyNumberIsInItsNotation()
    {
        QScopedPointer<RectDiagram> d(makeDiagram<RectDiagram>(kRect, "v", "<Mkr 2000 100 -100 3 0 0>", complex));
        QVERIFY(d);
        Marker* m = markerOf(d.data());
        QVERIFY(m != nullptr);
        QCOMPARE(m->notation, -1);
        QCOMPARE(d->notation, Notation::Engineering);
        QCOMPARE(m->Text, QStringLiteral("x: 2.000k\nv: 1.500k-j25.000u"));   // the diagram's
        const struct {
            int notation;
            const char* text;
        } cases[] = {
            {int(Notation::Automatic), "x: 2e+03\nv: 1.5e+03-j2.5e-05"},
            {int(Notation::Scientific), "x: 2.000e3\nv: 1.500e3-j2.500e-5"},
            {int(Notation::EngineeringExponent), "x: 2.000e3\nv: 1.500e3-j25.000e-6"},
            {int(Notation::Engineering), "x: 2.000k\nv: 1.500k-j25.000u"},
            {int(Notation::Decimal), "x: 2000.000\nv: 1500.000-j0.000"},
        };
        for (const auto& c : cases) {
            m->notation = c.notation;
            m->createText();
            QCOMPARE(m->Text, QString::fromUtf8(c.text));
        }
        m->notation = int(Notation::Power);
        m->createText();
        QCOMPARE(m->Text, QString::fromUtf8("x: 2.000×10³\nv: 1.500×10³-j2.500×10⁻⁵"));
        // Automatic is the old text, character for character.
        m->notation = int(Notation::Automatic);
        m->createText();
        QVERIFY(m->Text.endsWith(misc::complexRect(1500, -2.5e-5, 3)));
        m->numMode = nM_Deg;
        m->createText();
        QVERIFY(m->Text.endsWith(misc::complexDeg(1500, -2.5e-5, 3)));
        m->numMode = nM_Rad;
        m->createText();
        QVERIFY(m->Text.endsWith(misc::complexRad(1500, -2.5e-5, 3)));
        // Magnitude and angle in a notation: both of them.
        m->notation = int(Notation::Scientific);
        m->numMode = nM_Deg;
        m->createText();
        QCOMPARE(m->Text, QString::fromUtf8("x: 2.000e3\nv: 1.500e3 / -9.549e-7°"));
        // Back to the diagram's, which follows the diagram.
        m->notation = -1;
        m->numMode = nM_Rect;
        d->notation = Notation::Scientific;
        m->createText();
        QCOMPARE(m->Text, QStringLiteral("x: 2.000e3\nv: 1.500e3-j2.500e-5"));
        QCOMPARE(m->shownNotation(), Notation::Scientific);
    }

    // A value in dB (a log y axis in dB): in the notation, and no blank
    // line under it (the engineering notation always left one).
    void aValueInDecibels()
    {
        QScopedPointer<RectDiagram> d(makeDiagram<RectDiagram>(kRect, "v", "<Mkr 2000 100 -100 3 0 0>", complex));
        QVERIFY(d);
        d->yAxis.log = true;
        d->yAxis.Units = Axis::dbUnits;
        Marker* m = markerOf(d.data());
        m->createText();
        QCOMPARE(m->Text, QStringLiteral("x: 2.000k\nv: 63.522"));   // 20 log10(1500)
        m->notation = int(Notation::Scientific);
        m->createText();
        QCOMPARE(m->Text, QStringLiteral("x: 2.000e3\nv: 6.352e1"));
    }

    // A Smith chart's marker writes its impedance in its notation too.
    void aSmithChartsImpedance()
    {
        const QString s = writeDataset(dir.filePath("s.dat"), "S[1,1]", {{0, 0}, {0.2, 0.1}, {0, 0}});
        QScopedPointer<SmithDiagram> d(makeDiagram<SmithDiagram>(kSmith, "S[1,1]", "<Mkr 2000 100 -100 3 0 0>", s));
        QVERIFY(d);
        Marker* m = markerOf(d.data());
        QVERIFY(m != nullptr);
        // Z = 50 (1 + S) / (1 - S) = 73.077+j15.385 for S = 0.2+j0.1.
        m->notation = int(Notation::Scientific);
        m->createText();
        QVERIFY2(m->Text.endsWith(QStringLiteral("\nZ[1,1]: 7.308e1+j1.538e1")), qPrintable(m->Text));
        m->notation = int(Notation::Automatic);
        m->createText();
        QVERIFY2(m->Text.endsWith(QStringLiteral("\nZ[1,1]: ") + misc::complexRect(73.07692307692308, 15.384615384615385, 3)), qPrintable(m->Text));
    }

    // Saved after the colours, only when it has one of its own; a line of
    // before (or with a notation there is not) is the diagram's; copied.
    void itIsSavedAndReadBack()
    {
        QScopedPointer<RectDiagram> d(makeDiagram<RectDiagram>(kRect, "v", "<Mkr 2000 100 -100 3 0 0>", complex));
        Marker* m = markerOf(d.data());
        QCOMPARE(m->save(), QStringLiteral("<Mkr 2000 100 -100 3 0 0>"));   // as before
        m->notation = int(Notation::Power);
        QCOMPARE(m->save(), QStringLiteral("<Mkr 2000 100 -100 3 0 0 2 - - 5>"));
        m->textColor = QColor("#112233");
        QCOMPARE(m->save(), QStringLiteral("<Mkr 2000 100 -100 3 0 0 2 #112233 - 5>"));
        Marker read(const_cast<Graph*>(m->pGraph));
        QVERIFY(read.load(m->save()));
        QCOMPARE(read.notation, int(Notation::Power));
        QCOMPARE(read.textColor, QColor("#112233"));
        QVERIFY(read.load("<Mkr 2000 100 -100 3 0 0 2 - ->"));
        QCOMPARE(read.notation, -1);
        QVERIFY(read.load("<Mkr 2000 100 -100 3 0 0 2 - - 9>"));
        QCOMPARE(read.notation, -1);
        QVERIFY(read.load("<Mkr 2000 100 -100 3 0 0 2 - - 0>"));
        QCOMPARE(read.notation, int(Notation::Automatic));
        QScopedPointer<Marker> copy(m->sameNewOne(const_cast<Graph*>(m->pGraph)));
        QCOMPARE(copy->notation, int(Notation::Power));
    }

    // The dialog: the diagram's (named) or one of the six, each with its
    // examples; OK sets it and writes the text anew.
    void theDialogChoosesIt()
    {
        QScopedPointer<RectDiagram> d(makeDiagram<RectDiagram>(kRect, "v", "<Mkr 2000 100 -100 3 0 0>", complex));
        Marker* m = markerOf(d.data());
        QPointer<MarkerDialog> dialog = new MarkerDialog(m);
        QComboBox* box = dialog->findChild<QComboBox*>("markerNotation");
        QVERIFY(box != nullptr && box == dialog->NotationBox);
        QCOMPARE(box->count(), 7);
        QCOMPARE(box->itemData(0).toInt(), -1);
        QVERIFY2(box->itemText(0).contains("now engineering, SI prefixes"), qPrintable(box->itemText(0)));
        QCOMPARE(box->currentIndex(), 0);
        QVERIFY(box->itemText(box->findData(int(Notation::Power))).contains(QString::fromUtf8("1.5×10³")));
        QVERIFY(!dialog->Precision->toolTip().isEmpty());
        if (const QString shots = qEnvironmentVariable("QUCS_TEST_SHOTS"); !shots.isEmpty()) {
            dialog->adjustSize();
            dialog->grab().save(shots + "/marker-dialog.png");
        }
        box->setCurrentIndex(box->findData(int(Notation::Scientific)));
        QPushButton* ok = nullptr;
        for (QPushButton* b : dialog->findChildren<QPushButton*>())
            if (b->text() == "OK") ok = b;
        QVERIFY(ok != nullptr);
        ok->click();
        QCOMPARE(m->notation, int(Notation::Scientific));
        QCOMPARE(m->Text, QStringLiteral("x: 2.000e3\nv: 1.500e3-j2.500e-5"));
        // Opened again: its own is chosen.
        QPointer<MarkerDialog> again = new MarkerDialog(m);
        QCOMPARE(again->NotationBox->currentData().toInt(), int(Notation::Scientific));
        delete again;
    }
};

QTEST_MAIN(TestMarkerNotation)
#include "test_marker_notation.moc"
