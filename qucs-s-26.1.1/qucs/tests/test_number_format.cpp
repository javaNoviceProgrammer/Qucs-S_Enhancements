/*
 * The numbers of a diagram (numberformat.h): automatic, decimal,
 * scientific, scientific with a power of ten, engineering with SI
 * prefixes (as Qucs always wrote them) and with an exponent; the places
 * after the point; the axis labels of a diagram in each; the notation and
 * the places saved with the diagram and read back, from older files too;
 * the diagram dialog's choices.
 */
#include <QtTest>
#include <QComboBox>
#include <QSpinBox>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "numberformat.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/diagramdialog.h"

using namespace qucs_s::numberformat;

namespace {

// A rectangular diagram whose x axis runs from \a low to \a high in steps
// of \a step, with the notation field \a notation and, when given, the
// places field.
QString rectLine(double low, double step, double high, const QString& notation, const QString& places = QString())
{
    QString line = QStringLiteral("<Rect 0 300 600 300 3 #c0c0c0 1 00 0 %1 %2 %3 1 -1 0.5 1 1 -0.002 0.01 0.02 "
                                  "315 0 225 %4 0 0 0")
                       .arg(low).arg(step).arg(high).arg(notation);
    if (!places.isEmpty())
        line += QLatin1Char(' ') + places;
    return line + QStringLiteral(" \"\" \"\" \"\">");
}

RectDiagram* load(const QString& line)
{
    QString body = "</Rect>\n";
    QTextStream stream(&body, QIODevice::ReadOnly);
    auto* d = new RectDiagram();
    if (!d->load(line, &stream)) {
        delete d;
        return nullptr;
    }
    return d;
}

// The texts of the diagram's axes.
QStringList labels(Diagram* d)
{
    d->calcDiagram();
    QStringList out;
    for (const Text* t : d->Texts)
        out << t->s;
    return out;
}

} // namespace

class TestNumberFormat : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void automaticIsWhatItWas()
    {
        for (const double v : {0.5, 250.0, 1500.0, 2.5e-5, -3e9, 1.0 / 3.0})
            QCOMPARE(format(v, Notation::Automatic), misc::StringNiceNum(v));
        QCOMPARE(format(0.5, Notation::Automatic, 2), QString("0.50"));
        QCOMPARE(format(1500, Notation::Automatic, 2), QString("1.50e3"));
    }

    void engineeringIsWhatItWas()
    {
        // As misc::num2str always wrote axis labels and markers.
        for (const double v : {0.5, 0.1, 1500.0, 2.5e-5, -3e9, 1e15, 7.0, 0.3})
            QCOMPARE(format(v, Notation::Engineering), misc::num2str(v));
        QCOMPARE(format(1500, Notation::Engineering), QString("1.5k"));
        QCOMPARE(format(2.5e-5, Notation::Engineering), QString("25u"));
        QCOMPARE(format(1234.5678, Notation::Engineering, 2), QString("1.23k"));
    }

    void decimal()
    {
        QCOMPARE(format(1500, Notation::Decimal), QString("1500"));
        QCOMPARE(format(2.5e-5, Notation::Decimal), QString("0.000025"));
        QCOMPARE(format(1e-20, Notation::Decimal), QString("0.00000000000000000001"));
        QCOMPARE(format(1e15, Notation::Decimal), QString("1000000000000000"));
        QCOMPARE(format(0.1 + 0.2, Notation::Decimal), QString("0.3"));
        QCOMPARE(format(-2.5, Notation::Decimal), QString("-2.5"));
        QCOMPARE(format(1.5, Notation::Decimal, 3), QString("1.500"));
        // The step of an axis lines the labels up.
        QCOMPARE(format(0.5, Notation::Decimal, -1, 0.25), QString("0.50"));
        QCOMPARE(format(0.0, Notation::Decimal, -1, 0.25), QString("0.00"));
        QCOMPARE(format(3.0, Notation::Decimal, -1, 1.0), QString("3"));
        QCOMPARE(format(0.30000000000000004, Notation::Decimal, -1, 0.1), QString("0.3"));
    }

    void scientific()
    {
        QCOMPARE(format(1500, Notation::Scientific), QString("1.5e3"));
        QCOMPARE(format(0.5, Notation::Scientific), QString("5e-1"));
        QCOMPARE(format(2.5e-5, Notation::Scientific), QString("2.5e-5"));
        QCOMPARE(format(-1500, Notation::Scientific), QString("-1.5e3"));
        QCOMPARE(format(7, Notation::Scientific), QString("7e0"));
        QCOMPARE(format(9.99999999999999, Notation::Scientific), QString("1e1"));
        QCOMPARE(format(9.999, Notation::Scientific, 2), QString("1.00e1"));   // rounded up a decade
        QCOMPARE(format(1500, Notation::Scientific, 3), QString("1.500e3"));
    }

    void powerOfTen()
    {
        QCOMPARE(format(1500, Notation::Power), QString::fromUtf8("1.5×10³"));
        QCOMPARE(format(2.5e-5, Notation::Power), QString::fromUtf8("2.5×10⁻⁵"));
        QCOMPARE(format(1e6, Notation::Power), QString::fromUtf8("10⁶"));
        QCOMPARE(format(-1e-3, Notation::Power), QString::fromUtf8("-10⁻³"));
        QCOMPARE(format(1e-12, Notation::Power), QString::fromUtf8("10⁻¹²"));
        QCOMPARE(format(7, Notation::Power), QString("7"));
    }

    void engineeringExponent()
    {
        QCOMPARE(format(1500, Notation::EngineeringExponent), QString("1.5e3"));
        QCOMPARE(format(0.5, Notation::EngineeringExponent), QString("500e-3"));
        QCOMPARE(format(2.5e-5, Notation::EngineeringExponent), QString("25e-6"));
        QCOMPARE(format(250, Notation::EngineeringExponent), QString("250"));
        QCOMPARE(format(-4.7e-9, Notation::EngineeringExponent), QString("-4.7e-9"));
        QCOMPARE(format(999.99, Notation::EngineeringExponent, 1), QString("1.0e3"));
    }

    void zeroAndTheRest()
    {
        for (int n = 0; n <= 5; ++n)
            QCOMPARE(format(0.0, fromInt(n)), QString("0"));
        QCOMPARE(format(-0.0, Notation::Scientific), QString("0"));
        QCOMPARE(format(0.0, Notation::Scientific, 2), QString("0.00"));
        QCOMPARE(format(qInf(), Notation::Decimal), QString("inf"));
        QCOMPARE(fromInt(4), Notation::Decimal);
        QCOMPARE(fromInt(6), Notation::Automatic);
        QCOMPARE(fromInt(-1), Notation::Automatic);
    }

    void theChoicesShowExamples()
    {
        const auto list = choices();
        QCOMPARE(list.size(), 6);
        QCOMPARE(list.first().first, Notation::Automatic);
        QString decimal;
        for (const auto& [notation, text] : list)
            if (notation == Notation::Decimal) decimal = text;
        QVERIFY2(decimal.contains("1500") && decimal.contains("0.000025"), qPrintable(decimal));
    }

    // ---- a diagram -----------------------------------------------------

    void theAxisLabelsInEachNotation()
    {
        QScopedPointer<RectDiagram> d(load(rectLine(0, 0.25, 1, "4")));   // decimal
        QVERIFY(d);
        QCOMPARE(d->notation, Notation::Decimal);
        QStringList text = labels(d.get());
        for (const char* label : {"0.00", "0.25", "0.50", "0.75", "1.00"})
            QVERIFY2(text.contains(label), qPrintable(text.join(" | ")));

        d->notation = Notation::Scientific;
        text = labels(d.get());
        for (const char* label : {"0", "2.5e-1", "5e-1", "1e0"})
            QVERIFY2(text.contains(label), qPrintable(text.join(" | ")));

        d->notation = Notation::Engineering;   // as before
        text = labels(d.get());
        for (const char* label : {"0", "250m", "0.5", "1"})
            QVERIFY2(text.contains(label), qPrintable(text.join(" | ")));

        d->notation = Notation::Decimal;
        d->notationDecimals = 1;
        text = labels(d.get());
        for (const char* label : {"0.0", "0.3", "0.5", "0.8", "1.0"})   // 0.25 rounded up
            QVERIFY2(text.contains(label), qPrintable(text.join(" | ")));
    }

    // A diagram loaded without graphs (or with data that has not changed)
    // showed the labels of a new diagram - 0 to 1, engineering - until
    // something laid it out again.
    void aLoadedDiagramShowsItsOwnAxes()
    {
        QScopedPointer<RectDiagram> d(load(rectLine(0, 250000, 1e6, "4")));
        QVERIFY(d);
        d->loadGraphData(QStringLiteral("no-such-dataset.dat"));
        QStringList text;
        for (const Text* t : d->Texts)
            text << t->s;
        for (const char* label : {"0", "250000", "500000", "750000", "1000000"})
            QVERIFY2(text.contains(label), qPrintable(text.join(" | ")));
    }

    void savedWithTheDiagram()
    {
        QScopedPointer<RectDiagram> d(load(rectLine(0, 0.25, 1, "1")));
        QVERIFY(d);
        d->notation = Notation::Power;
        d->notationDecimals = 3;
        const QString line = d->save().section('\n', 0, 0);
        QCOMPARE(line.section(' ', 24, 24), QString("5"));
        QCOMPARE(line.section(' ', 28, 28), QString("3"));
        QVERIFY(line.endsWith("\"\" \"\" \"\">"));
        QScopedPointer<RectDiagram> again(load(line));
        QVERIFY(again);
        QCOMPARE(again->notation, Notation::Power);
        QCOMPARE(again->notationDecimals, 3);
        QCOMPARE(again->legendPos, int(Diagram::LegendOff));   // the field before it still lands
    }

    void olderFilesRead()
    {
        // "0" and "1" were scientific and engineering; no places field.
        QScopedPointer<RectDiagram> sci(load(rectLine(0, 0.25, 1, "0")));
        QVERIFY(sci);
        QCOMPARE(sci->notation, Notation::Automatic);
        QCOMPARE(sci->notationDecimals, -1);
        QScopedPointer<RectDiagram> eng(load(rectLine(0, 0.25, 1, "1")));
        QVERIFY(eng);
        QCOMPARE(eng->notation, Notation::Engineering);
        // Out of range or damaged: automatic, auto places, and it loads.
        QScopedPointer<RectDiagram> bad(load(rectLine(0, 0.25, 1, "9", "99")));
        QVERIFY(bad);
        QCOMPARE(bad->notation, Notation::Automatic);
        QCOMPARE(bad->notationDecimals, -1);
        QScopedPointer<RectDiagram> junk(load(rectLine(0, 0.25, 1, "x", "y")));
        QVERIFY(junk);
        QCOMPARE(junk->notationDecimals, -1);
    }

    void theDialogOffersThemAll()
    {
        QScopedPointer<RectDiagram> d(load(rectLine(0, 0.25, 1, "4", "2")));
        QVERIFY(d);
        auto* dialog = new DiagramDialog(d.get(), nullptr);   // WA_DeleteOnClose
        QComboBox* notation = nullptr;
        for (QComboBox* box : dialog->findChildren<QComboBox*>())
            if (box->findData(int(Notation::Power)) >= 0 && box->count() == 6) notation = box;
        QVERIFY(notation != nullptr);
        QCOMPARE(notation->currentData().toInt(), int(Notation::Decimal));
        QSpinBox* places = nullptr;
        for (QSpinBox* box : dialog->findChildren<QSpinBox*>())
            if (box->specialValueText() == "auto") places = box;
        QVERIFY(places != nullptr);
        QCOMPARE(places->value(), 2);

        notation->setCurrentIndex(notation->findData(int(Notation::EngineeringExponent)));
        places->setValue(-1);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->notation, Notation::EngineeringExponent);
        QCOMPARE(d->notationDecimals, -1);
        dialog->close();
    }
};

int main(int argc, char** argv)
{
    int one = 1;
    QApplication app(one, argv);
    TestNumberFormat test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_number_format.moc"
