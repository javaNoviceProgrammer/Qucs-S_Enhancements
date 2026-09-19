/*
 * The simulator-output parsers on damaged input (found by
 * scripts/ci/fuzz-simout.py). A raw file's header counts are claims, not
 * facts: the parsers must be bounded by what the file really contains,
 * and every sample row they hand to convertToQucsData() must be complete.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QElapsedTimer>

#include "config.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"

class TestSimout : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    Schematic* sch = nullptr;
    Ngspice* kernel = nullptr;

    QString write(const QString& name, const QByteArray& bytes)
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(bytes);
        return path;
    }

    static QByteArray header(const QString& counts, const QStringList& vars, bool complex = false)
    {
        QByteArray h = "Title: test\nDate: today\nPlotname: AC Analysis\n";
        h += complex ? "Flags: complex\n" : "Flags: real\n";
        h += counts.toUtf8();
        h += "Variables:\n";
        for (int i = 0; i < vars.size(); ++i)
            h += QStringLiteral("\t%1\t%2\tvoltage\n").arg(i).arg(vars[i]).toUtf8();
        return h;
    }

    static QByteArray doubles(std::initializer_list<double> values)
    {
        QByteArray b;
        QDataStream s(&b, QIODevice::WriteOnly);
        s.setByteOrder(QDataStream::LittleEndian);
        s.setFloatingPointPrecision(QDataStream::DoublePrecision);
        for (double v : values) s << v;
        return b;
    }

    struct Parsed {
        QList<QList<double>> points;
        QStringList vars, extra;
        QList<int> dims;
        bool complex = false;
    };

    Parsed parseRaw(const QString& path)
    {
        Parsed p;
        kernel->parseNgSpiceSimOutput(path, p.points, p.vars, p.complex, p.extra, p.dims);
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.maxUndo = 20;
        QucsSettings.S4Qworkdir = dir.path();
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        sch = new Schematic(nullptr, QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch->load());
        kernel = new Ngspice(sch);
    }

    void cleanupTestCase()
    {
        delete kernel;
        delete sch;
    }

    void genuineBinaryRawFile()
    {
        const QString path = write("ok.plot",
            header("No. Variables: 3\nNo. Points: 2\n", {"frequency", "v(a)", "v(b)"})
            + "Binary:\n" + doubles({1, 10, 100, 2, 20, 200}));
        const Parsed p = parseRaw(path);
        QCOMPARE(p.vars, QStringList({"frequency", "v(a)", "v(b)"}));
        QCOMPARE(p.points.size(), 2);
        QCOMPARE(p.points[1], QList<double>({2, 20, 200}));
    }

    void complexRawFileRows()
    {
        const QString path = write("cx.plot",
            header("No. Variables: 2\nNo. Points: 1\n", {"frequency", "v(a)"}, true)
            + "Binary:\n" + doubles({1, 0, 3, 4}));
        const Parsed p = parseRaw(path);
        QVERIFY(p.complex);
        QCOMPARE(p.points.size(), 1);
        QCOMPARE(p.points[0], QList<double>({1, 3, 4}));   // indep, re, im
    }

    // "No. Variables: 2147483647" made the header loop run two billion
    // times on an empty stream (and grow a list that size).
    void absurdVariableCountIsBoundedByTheFile()
    {
        const QString path = write("vars.plot",
            header("No. Variables: 2147483647\nNo. Points: 1\n", {"frequency", "v(a)"})
            + "Binary:\n" + doubles({1, 10}));
        QElapsedTimer t; t.start();
        const Parsed p = parseRaw(path);
        QVERIFY2(t.elapsed() < 5000, "header parsing must not scale with the claimed count");
        QVERIFY(p.vars.size() <= 4);                // the two names (+ what follows), not 2^31
    }

    void absurdPointCountIsBoundedByTheBytes()
    {
        const QString path = write("points.plot",
            header("No. Variables: 2\nNo. Points: 2000000000\n", {"frequency", "v(a)"})
            + "Binary:\n" + doubles({1, 10, 2, 20}));
        QElapsedTimer t; t.start();
        const Parsed p = parseRaw(path);
        QVERIFY(t.elapsed() < 5000);
        QCOMPARE(p.points.size(), 2);
    }

    void truncatedBinarySectionKeepsOnlyCompleteRows()
    {
        const QString path = write("short.plot",
            header("No. Variables: 3\nNo. Points: 2\n", {"frequency", "v(a)", "v(b)"})
            + "Binary:\n" + doubles({1, 10, 100, 2, 20}));   // second row incomplete
        const Parsed p = parseRaw(path);
        QCOMPARE(p.points.size(), 1);
        for (const auto& row : p.points) QCOMPARE(row.size(), 3);
    }

    void asciiValuesWithAMissingLineDropTheRow()
    {
        const QString path = write("ascii.plot",
            header("No. Variables: 2\nNo. Points: 2\n", {"frequency", "v(a)"})
            + "Values:\n0\t1.0\n\t10.0\n\t100.0\n1\t2.0\n");   // second point cut off
        const Parsed p = parseRaw(path);
        for (const auto& row : p.points) QCOMPARE(row.size(), p.points.first().size());
    }

    // parseSTEPOutput skipped an operating-point plot by reading lines until
    // the next "Plotname:", forever when there was none.
    void operatingPointWithoutAFollowingPlotEnds()
    {
        const QString path = write("op_swp.plot",
            "Title: t\nPlotname: DC operating point\nFlags: real\nNo. Variables: 1\nNo. Points: 1\n"
            "Variables:\n\t0\tv(a)\tvoltage\nBinary:\n" + doubles({1}));
        QList<QList<double>> points; QStringList vars, extra; QList<int> dims; bool complex = false;
        QElapsedTimer t; t.start();
        kernel->parseSTEPOutput(path, points, vars, complex, extra, dims);
        QVERIFY(t.elapsed() < 5000);
    }

    void headerOnlyEmptyAndMissingFiles()
    {
        QCOMPARE(parseRaw(write("hdr.plot", header("No. Variables: 3\nNo. Points: 5\n", {"a", "b", "c"}))).points.size(), 0);
        QCOMPARE(parseRaw(write("empty.plot", "")).points.size(), 0);
        QCOMPARE(parseRaw(dir.filePath("missing.plot")).points.size(), 0);
        QCOMPARE(parseRaw(write("junk.plot", QByteArray(4096, '\xff'))).points.size(), 0);
    }
};

int main(int argc, char** argv)
{
    int one = 1;
    QApplication app(one, argv);
    TestSimout test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_simout.moc"
