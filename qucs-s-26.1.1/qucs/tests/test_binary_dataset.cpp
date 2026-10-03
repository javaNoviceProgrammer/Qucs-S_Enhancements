/*
 * Binary datasets (datasetfile.h): a large run's results kept as the
 * simulator's doubles, not converted to text. The writers and the reader;
 * the same numbers from each kind of simulator output, text or binary;
 * every digit; a damaged file refused; a large run binary, its raw file not
 * kept twice, and quick; a diagram, Claude's dataset and Save as Text
 * reading a binary dataset as they read text.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QLineEdit>
#include <QStandardPaths>

#include <cmath>
#include <cstring>
#include <limits>

#ifndef Q_OS_WIN
#include <sys/resource.h>
#endif
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <unistd.h>
#endif

#include "config.h"
#include "dataset.h"
#include "dataexport.h"
#include "datasetfile.h"
#include "optimization.h"
#include "main.h"
#include "qucs.h"
#include "dialogs/importdialog.h"
#include "misc.h"
#include "module.h"
#include "schematic.h"
#include "diagrams/graph.h"
#include "diagrams/rectdiagram.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace df = qucs_s::datasetfile;
namespace ds = qucs_s::dataset;

namespace {

// The most memory of the process's own it has held (not counting the pages
// of files it maps, which the system takes back as it needs): macOS's
// footprint; else the resident size at its highest. In MB.
double peakFootprintMB()
{
#ifdef Q_OS_MACOS
    rusage_info_v4 info{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) == 0)
        return info.ri_lifetime_max_phys_footprint / 1048576.0;
#endif
#ifndef Q_OS_WIN
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_maxrss / 1024.0;   // (KB on Linux)
#else
    return -1;
#endif
}

class KernelProbe : public Ngspice
{
public:
    using Ngspice::Ngspice;
    void setOutputs(const QStringList& files) { a_output_files = files; }
};

QByteArray doubles(const QVector<double>& values)
{
    QByteArray b;
    QDataStream s(&b, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);
    for (double v : values) s << v;
    return b;
}

// A raw file of ngspice's: a plot of \a vars, binary (or ASCII), real or
// complex, its values point by point (a complex one's in pairs).
QByteArray rawPlot(const QString& plotname, const QStringList& vars, const QVector<double>& values, int points,
                   bool complex, bool binary = true, const QStringList& dims = {})
{
    QByteArray raw = "Title: test\nDate: today\nPlotname: " + plotname.toUtf8() + "\n";
    raw += complex ? "Flags: complex\n" : "Flags: real\n";
    raw += "No. Variables: " + QByteArray::number(vars.size()) + "\nNo. Points: " + QByteArray::number(points) + "\n";
    raw += "Variables:\n";
    for (int i = 0; i < vars.size(); ++i) {
        raw += QStringLiteral("\t%1\t%2\tvoltage").arg(i).arg(vars[i]).toUtf8();
        if (dims.contains(vars[i])) raw += " dims=2";
        raw += "\n";
    }
    if (binary) return raw + "Binary:\n" + doubles(values);
    raw += "Values:\n";
    const int w = complex ? 2 : 1;
    for (int p = 0; p < points; ++p)
        for (int v = 0; v < vars.size(); ++v) {
            const qsizetype at = (qsizetype(p) * vars.size() + v) * w;
            raw += v == 0 ? QByteArray::number(p) + "\t" : QByteArray("\t");
            raw += QByteArray::number(values.value(at), 'g', 17);
            if (complex) raw += "," + QByteArray::number(values.value(at + 1), 'g', 17);
            raw += "\n";
        }
    return raw;
}

// Two datasets' variables the same: names, kinds, dependencies, and every
// value to the bit (NaN where NaN).
bool sameNumbers(const ds::Dataset& a, const ds::Dataset& b, QString* why)
{
    const auto bits = [](double x, double y) { return (std::isnan(x) && std::isnan(y)) || std::memcmp(&x, &y, 8) == 0; };
    if (a.variables().size() != b.variables().size()) {
        *why = QStringLiteral("%1 variables against %2").arg(a.variables().size()).arg(b.variables().size());
        return false;
    }
    for (int i = 0; i < a.variables().size(); ++i) {
        const ds::Variable& x = a.variables().at(i);
        const ds::Variable& y = b.variables().at(i);
        if (x.name != y.name || x.independent != y.independent || x.dependencies != y.dependencies
            || x.writtenComplex != y.writtenComplex || x.re.size() != y.re.size() || x.im.size() != y.im.size()) {
            *why = QStringLiteral("%1 (%2 values) against %3 (%4 values)").arg(x.name).arg(x.re.size()).arg(y.name).arg(y.re.size());
            return false;
        }
        for (int k = 0; k < x.re.size(); ++k)
            if (!bits(x.re.at(k), y.re.at(k)) || (k < x.im.size() && !bits(x.im.at(k), y.im.at(k)))) {
                *why = QStringLiteral("%1[%2]: %3 against %4").arg(x.name).arg(k).arg(x.re.at(k), 0, 'g', 17).arg(y.re.at(k), 0, 'g', 17);
                return false;
            }
    }
    return true;
}

// The same blocks into a writer: real, complex, text values (a number, a
// complex one, one that is no number), a block real then complex, an empty
// one, NaN and infinities, -0, Monte Carlo's text blocks.
void sameBlocks(df::Writer& w)
{
    w.begin(QStringLiteral("indep time 4"));
    for (double t : {0.0, 1e-9, 2.5e-9, 1.0 / 3.0}) w.real(t);
    w.end();
    w.begin(QStringLiteral("dep tran.v(out) time"));
    for (double v : {15.000001, -0.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) w.real(v);
    w.end();
    w.begin(QStringLiteral("indep frequency 3"));
    for (double f : {1.0, 10.0, 100.0}) w.real(f);
    w.end();
    w.begin(QStringLiteral("dep ac.v(out) frequency"));
    w.complex(0.5, -0.25);
    w.complex(3.14159265358979, 2.718281828459045);
    w.complex(-1e-300, 0);
    w.end();
    w.begin(QStringLiteral("dep ac.mixed frequency"));   // real, then complex: complex from there
    w.real(1);
    w.complex(2, 3);
    w.real(4);
    w.end();
    w.begin(QStringLiteral("indep Number 3"));
    w.text("1");
    w.text("2e3");
    w.text("X");   // a digital value: no number
    w.end();
    w.begin(QStringLiteral("dep tran.db(x) time"));   // complex, its imaginary parts 0: real in fact
    for (double v : {-52.0, -40.0, -3.0, 0.0}) w.complex(v, 0);
    w.end();
    w.begin(QStringLiteral("indep empty 0"));
    w.end();
    w.begin(QStringLiteral("dep tran.v(out) time"));   // a name twice: the last one read is the one found
    for (double v : {1.0, 2.0, 3.0, 4.0}) w.real(v);
    w.end();
    w.blocks(QStringLiteral("<indep run 2>\n  1\n  2\n</indep>\n<dep MC1.gain run>\n  1.5+j0.5\n  -2e-3-j1\n</dep>\n"));
}

QString write(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size()) return {};
    return path;
}

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestBinaryDataset : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    Schematic* sch = nullptr;

    // The outputs \a files of \a work as a dataset, text or binary.
    QString convert(const QString& work, const QStringList& files, const QString& dataset,
                    AbstractSpiceKernel::DatasetFormat format)
    {
        KernelProbe kernel(sch);
        kernel.setWorkdir(work);
        kernel.setOutputs(files);
        kernel.setDatasetFormat(format);
        return kernel.convertToQucsData(dataset);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        // (No simulator asked who it is when the window starts.)
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.XyceExecutable = "xyce";
        QucsSettings.SpiceOpusExecutable = "spiceopus";
        QucsSettings.S4Qworkdir = dir.filePath("work");
        QucsSettings.tempFilesDir.setPath(dir.filePath("work"));
        QVERIFY(QDir().mkpath(dir.filePath("work")));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        sch = new Schematic(nullptr, QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch->load());
    }

    void cleanupTestCase()
    {
        delete sch;
        sch = nullptr;
    }

    // The same blocks written as text and as binary read back the same, to
    // the bit; the binary one's index has them in their order.
    void textAndBinaryReadTheSame()
    {
        const QString textPath = dir.filePath("same.dat.ngspice"), binaryPath = dir.filePath("same.bin.dat.ngspice");
        {
            QSaveFile f(textPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            df::TextWriter w(&f);
            QVERIFY(w.empty());
            sameBlocks(w);
            QVERIFY(!w.empty());
            QString why;
            QVERIFY2(w.finish(&why), qPrintable(why));
            QVERIFY(f.commit());
        }
        {
            QSaveFile f(binaryPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            df::BinaryWriter w(&f);
            QVERIFY(w.empty());
            sameBlocks(w);
            QVERIFY(!w.empty());
            QString why;
            QVERIFY2(w.finish(&why), qPrintable(why));
            QVERIFY(f.commit());
        }
        QVERIFY(!df::isBinary(textPath));
        QVERIFY(df::isBinary(binaryPath));
        ds::Dataset text, binary;
        QString error, why;
        QVERIFY2(text.read(textPath, &error), qPrintable(error));
        QVERIFY2(binary.read(binaryPath, &error), qPrintable(error));
        QVERIFY2(sameNumbers(text, binary, &why), qPrintable(why));
        QCOMPARE(text.variables().size(), 11);
        QCOMPARE(binary.find("tran.v(out)")->re.at(0), 1.0);   // the last of its name
        QVERIFY(binary.find("ac.mixed")->isComplex());
        QCOMPARE(binary.find("ac.mixed")->im.at(0), 0.0);
        QVERIFY(binary.find("tran.db(x)")->writtenComplex);
        QCOMPARE(binary.find("tran.db(x)")->re.at(0), -52.0);
        QVERIFY(std::isnan(binary.find("Number")->re.at(2)));
        QCOMPARE(binary.find("MC1.gain")->im.at(1), -1.0);
        QCOMPARE(binary.find("empty")->size(), 0);

        // Blocks given written (a Monte Carlo's alone) are some.
        for (const bool asBinary : {false, true}) {
            QSaveFile f(dir.filePath("blocks only"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            std::unique_ptr<df::Writer> w;
            if (asBinary)
                w = std::make_unique<df::BinaryWriter>(&f);
            else
                w = std::make_unique<df::TextWriter>(&f);
            w->blocks(QStringLiteral("<indep run 2>\n  1\n  2\n</indep>\n"));
            QVERIFY(!w->empty());
            f.cancelWriting();
        }

        df::BinaryReader reader;
        QVERIFY2(reader.open(binaryPath, &error), qPrintable(error));
        QStringList names;
        for (const df::Block& b : reader.blocks()) names << b.name();
        QCOMPARE(names, (QStringList{"time", "tran.v(out)", "frequency", "ac.v(out)", "ac.mixed", "Number", "tran.db(x)",
                                     "empty", "tran.v(out)", "run", "MC1.gain"}));
        QCOMPARE(reader.find("tran.v(out)"), 8);
        QCOMPARE(reader.find("tran.v(out)", true), 1);
        QCOMPARE(reader.blocks().at(1).dependencies(), QStringList{"time"});
        QVERIFY(reader.blocks().at(3).complex);
        // Every block's values within the file, 8-byte aligned.
        for (const df::Block& b : reader.blocks()) QCOMPARE(b.offset % 8, 0);
    }

    // Every digit: a value read back is the double written - in a binary
    // dataset, and in text too (as many digits as it takes).
    void numbersAreReadBackExactly()
    {
        const QVector<double> values{15.000001, 15.0, 0.1, 1e-300, 4.9406564584124654e-324, 1.7976931348623157e308,
                                     std::nextafter(1.0, 2.0), 3.141592653589793, -2.718281828459045e-17};
        for (const bool binary : {false, true}) {
            const QString path = dir.filePath(binary ? "exact.bin" : "exact.txt");
            QSaveFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            std::unique_ptr<df::Writer> w;
            if (binary)
                w = std::make_unique<df::BinaryWriter>(&f);
            else
                w = std::make_unique<df::TextWriter>(&f);
            w->begin(QStringLiteral("indep v %1").arg(values.size()));
            for (double v : values) w->real(v);
            w->end();
            QVERIFY(w->finish(nullptr));
            QVERIFY(f.commit());
            ds::Dataset d;
            QVERIFY(d.read(path));
            const ds::Variable* v = d.find("v");
            QVERIFY(v != nullptr);
            QCOMPARE(v->size(), int(values.size()));
            for (int i = 0; i < values.size(); ++i)
                QVERIFY2(std::memcmp(&v->re.at(i), &values.at(i), 8) == 0,
                         qPrintable(QString::number(v->re.at(i), 'g', 17) + " for " + QString::number(values.at(i), 'g', 17)));
            // 1 uV on 15 V, as the dataset has it: the double's own difference.
            QCOMPARE(v->re.at(0) - v->re.at(1), 15.000001 - 15.0);
        }
    }

    // A damaged binary dataset is refused, whatever is wrong with it - its
    // index, its last line, a block that does not fit the file - and none of
    // the readers reads past its end (ASan watches).
    void aDamagedBinaryDatasetIsRefused()
    {
        const QString good = dir.filePath("good.dat.ngspice");
        {
            QSaveFile f(good);
            QVERIFY(f.open(QIODevice::WriteOnly));
            df::BinaryWriter w(&f);
            w.begin(QStringLiteral("indep time 3"));
            for (double t : {0.0, 1.0, 2.0}) w.real(t);
            w.end();
            w.begin(QStringLiteral("dep tran.v(out) time"));
            for (double v : {5.0, 6.0, 7.0}) w.real(v);
            w.end();
            QVERIFY(w.finish(nullptr));
            QVERIFY(f.commit());
        }
        const QByteArray bytes = read(good);
        const qsizetype indexAt = bytes.indexOf("<index>\n");
        QVERIFY(indexAt > 64);
        // (The index's line of tran.v(out): "<dep tran.v(out) time> 88 3 r".)
        const QByteArray line = "<dep tran.v(out) time> 88 3 r\n";
        QVERIFY2(bytes.indexOf(line) > indexAt, bytes.mid(indexAt).constData());
        const auto withLine = [&](const QByteArray& other) {
            // The index's length in the last line kept true.
            QByteArray b = bytes;
            b.replace(line, other);
            const qint64 length = b.size() - 43 - indexAt;
            b.replace(b.size() - 17, 16, QByteArray::number(length, 16).rightJustified(16, '0'));
            return b;
        };
        const QList<QPair<QString, QByteArray>> damaged{
            {"cut in its values", bytes.left(80)},
            {"cut in its index", bytes.left(indexAt + 10)},
            {"cut in its last line", bytes.left(bytes.size() - 5)},
            {"its last line elsewhere", bytes.left(bytes.size() - 43) + "QDSINDEX 00000000000fffff 0000000000000010\n"},
            {"its last line garbled", bytes.left(bytes.size() - 43) + QByteArray(43, 'x')},
            {"more values than the file holds", withLine("<dep tran.v(out) time> 88 1000000 r\n")},
            {"a huge count", withLine("<dep tran.v(out) time> 88 4611686018427387904 r\n")},
            {"complex, its room for real", withLine("<dep tran.v(out) time> 88 4 c\n")},
            {"before the values", withLine("<dep tran.v(out) time> 8 3 r\n")},
            {"not aligned", withLine("<dep tran.v(out) time> 65 3 r\n")},
            {"a negative count", withLine("<dep tran.v(out) time> 88 -3 r\n")},
            {"neither real nor complex", withLine("<dep tran.v(out) time> 88 3 q\n")},
            {"no block's line", withLine("tran.v(out) 88 3 r\n")},
            {"a block of no kind", withLine("<var tran.v(out) time> 88 3 r\n")},
            {"one word", withLine("<dep> 88 3 r\n")},
            {"its first line not a dataset's", QByteArray("<Qucs Datasex") + bytes.mid(13)},
            {"something between its index and its last line",
             bytes.left(bytes.size() - 43) + "<dep extra time> 64 3 r\n" + bytes.right(43)},
        };
        for (const auto& [what, b] : damaged) {
            const QString path = write(dir.filePath("damaged.dat.ngspice"), b);
            QVERIFY(!path.isEmpty());
            df::BinaryReader r;
            QString error;
            QVERIFY2(!r.open(path, &error), qPrintable(what));
            QVERIFY2(!error.isEmpty(), qPrintable(what));
            ds::Dataset d;
            QVERIFY2(!d.read(path, &error), qPrintable(what));
            RectDiagram diagram;
            auto* g = new Graph(&diagram, QStringLiteral("ngspice/tran.v(out)"));
            diagram.Graphs.append(g);
            QCOMPARE(g->loadDatFile(dir.filePath("damaged.dat")), 0);
            QVERIFY(df::dependentNames(path).isEmpty());
        }
        // The good one reads.
        df::BinaryReader r;
        QVERIFY(r.open(good));
        QCOMPARE(r.blocks().size(), 2);
    }

    // Each kind of simulator output gives the same numbers as text and as
    // binary: a transient (binary raw, real), an AC (complex), an ASCII raw,
    // a parameter sweep (two plots), XSPICE digital nodes (dims=) - and an
    // operating point's devices beside them.
    void eachKindOfOutputGivesTheSameNumbers_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QByteArray>("raw");
        QTest::addColumn<QStringList>("expected");   // some of its variables
        const int n = 50;
        QVector<double> tran, ac, ascii, digital;
        for (int p = 0; p < n; ++p) {
            tran << p * 1e-9 << std::sin(p / 7.0) * 15 << 15.000001 + p * 1e-12 << -1.0 / (p + 1);
            ac << std::pow(10.0, p / 10.0) << 0 << std::cos(p * 0.1) << -std::sin(p * 0.1) << 1e-300 * p << 1.0 / 3.0;
            ascii << p * 0.5 << p * p * 1e-3;
            digital << p * 1e-9 << (p % 2) << (p % 3) * 1e-9 << ((p + 1) % 2);
        }
        QTest::newRow("transient") << QStringLiteral("spice4qucs.tran.plot")
                                   << rawPlot("Transient Analysis", {"time", "v(out)", "v(in)", "i(v1)"}, tran, n, false)
                                   << QStringList{"time", "tran.v(out)", "tran.v(in)", "tran.i(v1)"};
        QTest::newRow("ac") << QStringLiteral("spice4qucs.ac.plot")
                            << rawPlot("AC Analysis", {"frequency", "v(out)", "v(x1:n1)"}, ac, n, true)
                            << QStringList{"frequency", "ac.v(out)", "ac.v(x1_n1)"};
        QTest::newRow("ascii") << QStringLiteral("spice4qucs.tran.plot")
                               << rawPlot("Transient Analysis", {"time", "v(out)"}, ascii, n, false, false)
                               << QStringList{"time", "tran.v(out)"};
        QByteArray sweep = rawPlot("Transient Analysis", {"time", "v(out)"}, ascii.mid(0, 20), 10, false)
                           + rawPlot("Transient Analysis", {"time", "v(out)"}, ascii.mid(20, 20), 10, false);
        QTest::newRow("sweep") << QStringLiteral("spice4qucs.tran.plot") << sweep << QStringList{"time", "Number", "tran.v(out)"};
        QTest::newRow("digital") << QStringLiteral("spice4qucs.tran.plot")
                                 << rawPlot("Transient Analysis", {"time", "v(d)", "d_steps", "v(e)"}, digital, n, false, true, {"v(d)", "d_steps"})
                                 << QStringList{"time"};
    }

    void eachKindOfOutputGivesTheSameNumbers()
    {
        QFETCH(QString, file);
        QFETCH(QByteArray, raw);
        QFETCH(QStringList, expected);
        const QString work = dir.filePath("kinds");
        QVERIFY(QDir().mkpath(work));
        const QString textPath = work + "/text.dat.ngspice", binaryPath = work + "/binary.dat.ngspice";
        // An operating point's devices, beside every run.
        QVERIFY(!write(work + "/spice4qucs.op_dev", " BJT: Bipolar Junction Transistor\n     device                   q1\n"
                                                    "      model                   qn\n         ic   1.23456789012345e-03\n"
                                                    "        vbe   6.5e-01\n").isEmpty());
        QVERIFY(!write(work + "/" + file, raw).isEmpty());
        QString error = convert(work, {file}, textPath, AbstractSpiceKernel::DatasetFormat::Text);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(work + "/" + file));   // text: the raw file stays, as before
        QVERIFY(!df::isBinary(textPath));
        error = convert(work, {file}, binaryPath, AbstractSpiceKernel::DatasetFormat::Binary);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(df::isBinary(binaryPath));
        ds::Dataset text, binary;
        QVERIFY2(text.read(textPath, &error), qPrintable(error));
        QVERIFY2(binary.read(binaryPath, &error), qPrintable(error));
        QString why;
        QVERIFY2(sameNumbers(text, binary, &why), qPrintable(QString::fromUtf8(QTest::currentDataTag()) + ": " + why));
        for (const QString& name : expected) QVERIFY2(binary.find(name) != nullptr, qPrintable(name));
        QVERIFY(binary.find("@q1[ic]") != nullptr);   // (the operating point's, every digit)
        QCOMPARE(binary.find("@q1[ic]")->re.at(0), 1.23456789012345e-03);
        QFile::remove(work + "/spice4qucs.op_dev");
        QFile::remove(work + "/" + file);
    }

    // The numbers of a transient's binary raw file, as they are: the binary
    // dataset has the doubles the simulator wrote, every one, and the raw
    // file - taken whole into it - is not kept beside it.
    void aBinaryDatasetHasTheSimulatorsNumbers()
    {
        const QString work = dir.filePath("exact");
        QVERIFY(QDir().mkpath(work));
        const int n = 1000;
        QVector<double> values;
        for (int p = 0; p < n; ++p) values << p * 1e-9 << 15.0 + 1e-6 * std::sin(p) << std::nextafter(15.0, 16.0) * p;
        const QString raw = write(work + "/spice4qucs.tran.plot", rawPlot("Transient Analysis", {"time", "v(a)", "v(b)"}, values, n, false));
        QVERIFY(!raw.isEmpty());
        const QString error = convert(work, {"spice4qucs.tran.plot"}, work + "/amp.dat.ngspice", AbstractSpiceKernel::DatasetFormat::Binary);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(raw));
        ds::Dataset d;
        QVERIFY(d.read(work + "/amp.dat.ngspice"));
        const QStringList names{"time", "tran.v(a)", "tran.v(b)"};
        for (int v = 0; v < 3; ++v) {
            const ds::Variable* var = d.find(names.at(v));
            QVERIFY(var != nullptr);
            QCOMPARE(var->size(), n);
            for (int p = 0; p < n; ++p) QVERIFY(std::memcmp(&var->re.at(p), &values.at(p * 3 + v), 8) == 0);
        }
        QCOMPARE(d.find("tran.v(a)")->dependencies, QStringList{"time"});
    }

    // The settings choose: text up to their limit, binary above it; text
    // with binary off; text for a schematic whose Octave script reads it.
    void theSettingsChooseTextOrBinary()
    {
        const QString work = dir.filePath("choose");
        QVERIFY(QDir().mkpath(work));
        const auto run = [&](int points) {
            QVector<double> values;
            for (int p = 0; p < points; ++p) values << p << p * 2.0;
            write(work + "/spice4qucs.tran.plot", rawPlot("Transient Analysis", {"time", "v(out)"}, values, points, false));
            const QString error = convert(work, {"spice4qucs.tran.plot"}, work + "/r.dat.ngspice", AbstractSpiceKernel::DatasetFormat::Settings);
            return error.isEmpty() && df::isBinary(work + "/r.dat.ngspice");
        };
        const bool binaryWas = QucsSettings.DatasetBinary;
        const int limitWas = QucsSettings.DatasetTextLimitMB;
        QucsSettings.DatasetBinary = true;
        QucsSettings.DatasetTextLimitMB = 1;
        QVERIFY(!run(1000));        // 16 kB: text
        QVERIFY(run(70000));        // 1.1 MB: binary
        QucsSettings.DatasetTextLimitMB = 0;
        QVERIFY(run(10));           // 0 MB: every run's
        QucsSettings.DatasetBinary = false;
        QVERIFY(!run(70000));       // off: text
        QucsSettings.DatasetBinary = true;
        sch->setSimRunScript(true);   // an Octave script after the run
        QVERIFY(!run(70000));
        sch->setSimRunScript(false);
        sch->setSimOpenDpl(true);
        const QString display = sch->getDataDisplay();
        sch->setDataDisplay(QStringLiteral("post.m"));
        QVERIFY(!run(70000));
        sch->setDataDisplay(display);
        QVERIFY(run(70000));
        QucsSettings.DatasetBinary = binaryWas;
        QucsSettings.DatasetTextLimitMB = limitWas;
    }

    // A diagram's graph reads a binary dataset as it reads text: a real
    // curve, a complex one shown as its phase, a family over two variables,
    // an independent variable over its index, one over another (PlotVs), a
    // name alone found as a voltage - and nothing for one not there.
    void aGraphReadsBinaryAsItReadsText()
    {
        const auto blocks = [](df::Writer& w) {
            w.begin("indep time 5");
            for (int i = 0; i < 5; ++i) w.real(i * 1e-3);
            w.end();
            w.begin("indep Number 2");
            w.real(1);
            w.real(2);
            w.end();
            w.begin("dep tran.v(out) time");
            for (int i = 0; i < 5; ++i) w.real(std::sin(i) + 15.000001);
            w.end();
            w.begin("dep tran.v(fam) time Number");
            for (int i = 0; i < 10; ++i) w.real(i * 0.5);
            w.end();
            w.begin("dep tran.v(gain) time");
            for (int i = 0; i < 5; ++i) w.real(i * 3);
            w.end();
            w.begin("indep frequency 3");
            for (double f : {1.0, 2.0, 3.0}) w.real(f);
            w.end();
            w.begin("dep ac.v(out) frequency");
            w.complex(1, 1);
            w.complex(0, -2);
            w.complex(-3, 0.5);
            w.end();
            w.begin("dep tran.v(short) time");   // fewer values than time has: nothing
            for (int i = 0; i < 3; ++i) w.real(i);
            w.end();
            w.begin("dep tran.v(out) time");   // a name twice: a graph takes the first
            for (int i = 0; i < 5; ++i) w.real(-i);
            w.end();
        };
        const QString textBase = dir.filePath("graphtext"), binaryBase = dir.filePath("graphbin");
        for (const bool binary : {false, true}) {
            QSaveFile f((binary ? binaryBase : textBase) + ".dat.ngspice");
            QVERIFY(f.open(QIODevice::WriteOnly));
            std::unique_ptr<df::Writer> w;
            if (binary)
                w = std::make_unique<df::BinaryWriter>(&f);
            else
                w = std::make_unique<df::TextWriter>(&f);
            blocks(*w);
            QVERIFY(w->finish(nullptr));
            QVERIFY(f.commit());
        }
        const QStringList traces{"ngspice/tran.v(out)", "ngspice/tran.v(fam)", "ngspice/time", "ngspice/tran.v(out)@tran.v(gain)",
                                 "ngspice/tran.gain", "ngspice/ac.v(out)", "ngspice/tran.v(short)", "ngspice/nothing"};
        for (const QString& trace : traces) {
            for (const auto part : {Graph::ValuePart::Auto, Graph::ValuePart::Phase}) {
                RectDiagram d1, d2;
                auto* t = new Graph(&d1, trace);
                auto* b = new Graph(&d2, trace);
                d1.Graphs.append(t);
                d2.Graphs.append(b);
                t->valuePart = b->valuePart = part;
                const int loadedText = t->loadDatFile(textBase + ".dat");
                QCOMPARE(b->loadDatFile(binaryBase + ".dat"), loadedText);
                QVERIFY2(loadedText == (trace.endsWith("nothing") || trace.endsWith("short)") ? 0 : 2), qPrintable(trace));
                if (trace == "ngspice/tran.v(out)" && part == Graph::ValuePart::Auto) QVERIFY(b->cPointsY[2] > 15);   // the first of its name
                QCOMPARE(b->numAxes(), t->numAxes());
                QCOMPARE(b->countY, t->countY);
                for (unsigned a = 0; a < t->numAxes(); ++a) {
                    QCOMPARE(b->axisName(a), t->axisName(a));
                    QCOMPARE(b->count(a), t->count(a));
                    for (size_t k = 0; k < t->count(a); ++k) QCOMPARE(b->axis(a)->Points[k], t->axis(a)->Points[k]);
                    QCOMPARE(b->axis(a)->min(), t->axis(a)->min());
                    QCOMPARE(b->axis(a)->max(), t->axis(a)->max());
                }
                if (loadedText != 2) continue;
                const size_t values = t->count(0) * size_t(t->countY);
                for (size_t k = 0; k < 2 * values; ++k)
                    QVERIFY2(b->cPointsY[k] == t->cPointsY[k] || (std::isnan(b->cPointsY[k]) && std::isnan(t->cPointsY[k])),
                             qPrintable(trace + " " + QString::number(k)));
            }
        }
    }

    // Save as Text: a binary dataset written out as text has the same
    // numbers; so does a CSV export of each.
    void saveAsTextGivesTheSameNumbers()
    {
        const QString binaryPath = dir.filePath("export.dat.ngspice");
        {
            QSaveFile f(binaryPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            df::BinaryWriter w(&f);
            sameBlocks(w);
            QVERIFY(w.finish(nullptr));
            QVERIFY(f.commit());
        }
        const QString textPath = dir.filePath("export.txt");
        {
            QSaveFile f(textPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QString error;
            QVERIFY2(df::writeText(binaryPath, &f, &error), qPrintable(error));
            QVERIFY(f.commit());
        }
        QVERIFY(read(textPath).startsWith("<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 4>\n"));
        ds::Dataset text, binary;
        QVERIFY(text.read(textPath));
        QVERIFY(binary.read(binaryPath));
        QString why;
        QVERIFY2(sameNumbers(text, binary, &why), qPrintable(why));
        // A CSV of each (one table: the variables over one).
        for (const QString& name : {QStringLiteral("tran.v(out)"), QStringLiteral("ac.v(out)"), QStringLiteral("MC1.gain")}) {
            QByteArray fromText, fromBinary;
            qucs_s::dataexport::Options csv;
            QString error;
            QVERIFY2(qucs_s::dataexport::encode(text, {name}, csv, &fromText, nullptr, &error), qPrintable(error));
            QVERIFY2(qucs_s::dataexport::encode(binary, {name}, csv, &fromBinary, nullptr, &error), qPrintable(error));
            QVERIFY(fromText.count('\n') > 2);
            QCOMPARE(fromBinary, fromText);
        }
        // Not over the binary one itself, nor from a file that is none.
        QVERIFY(!df::writeText(textPath, nullptr));
    }

    // The window's Save as Text (the Content panel's menu, a binary dataset
    // opened): the text file where it is asked for, the same numbers.
    void theWindowSavesABinaryDatasetAsText()
    {
        const QString binaryPath = dir.filePath("window.dat.ngspice");
        {
            QSaveFile f(binaryPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            df::BinaryWriter w(&f);
            sameBlocks(w);
            QVERIFY(w.finish(nullptr));
            QVERIFY(f.commit());
        }
        QucsApp app(false);
        QucsMain = &app;
        const QString target = dir.filePath("window as text.dat");
        QVERIFY(app.saveDatasetAsText(binaryPath, target));
        QucsMain = nullptr;
        QVERIFY(!df::isBinary(target));
        ds::Dataset text, binary;
        QVERIFY(text.read(target));
        QVERIFY(binary.read(binaryPath));
        QString why;
        QVERIFY2(sameNumbers(text, binary, &why), qPrintable(why));
    }

    // Data Files > Convert of a binary dataset: its variables listed from
    // its index; the converter (qucsconv, which reads text) given it as
    // text. (A converter not there: nothing runs.)
    void theConverterIsGivenText()
    {
        const QString binaryPath = dir.filePath("convert.dat.ngspice");
        {
            QSaveFile f(binaryPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            df::BinaryWriter w(&f);
            sameBlocks(w);
            QVERIFY(w.finish(nullptr));
            QVERIFY(f.commit());
        }
        const QString converterWas = QucsSettings.Qucsconv;
        QucsSettings.Qucsconv = dir.filePath("no-such-qucsconv");
        {
            ImportDialog dialog(nullptr);
            auto* input = dialog.findChild<QLineEdit*>("importFile");
            auto* output = dialog.findChild<QLineEdit*>("outputFile");
            auto* type = dialog.findChild<QComboBox*>("inputType");
            auto* variables = dialog.findChild<QComboBox*>("outputData");
            auto* messages = dialog.findChild<QPlainTextEdit*>("messages");
            QVERIFY(input && output && type && variables && messages);
            input->setText(binaryPath);
            QCOMPARE(type->currentIndex(), 3);   // a Qucs dataset
            QStringList listed;
            for (int i = 0; i < variables->count(); ++i) listed << variables->itemText(i);
            QCOMPARE(listed, df::dependentNames(binaryPath));
            output->setText(dir.filePath("converted.csv"));
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotImport"));
            const QString log = messages->toPlainText();
            const QRegularExpressionMatch m = QRegularExpression(QStringLiteral(" -i (\\S*qucs-dataset-\\S+) ")).match(log);
            QVERIFY2(m.hasMatch(), qPrintable(log));
            const QString copy = m.captured(1);
            QVERIFY(!log.contains(" -i " + binaryPath));
            QVERIFY(QFileInfo::exists(copy));
            QVERIFY(!df::isBinary(copy));
            ds::Dataset text, binary;
            QVERIFY(text.read(copy));
            QVERIFY(binary.read(binaryPath));
            QString why;
            QVERIFY2(sameNumbers(text, binary, &why), qPrintable(why));
        }
        QucsSettings.Qucsconv = converterWas;
    }

    // The optimizer reads a binary dataset as it reads text.
    void theOptimizerReadsBinary()
    {
        for (const bool binary : {false, true}) {
            QSaveFile f(dir.filePath(binary ? "optimized.bin" : "optimized.txt"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            std::unique_ptr<df::Writer> w;
            if (binary)
                w = std::make_unique<df::BinaryWriter>(&f);
            else
                w = std::make_unique<df::TextWriter>(&f);
            sameBlocks(*w);
            QVERIFY(w->finish(nullptr));
            QVERIFY(f.commit());
        }
        QString error;
        const auto text = qucs_s::optimization::readDataset(dir.filePath("optimized.txt"), &error);
        const auto binary = qucs_s::optimization::readDataset(dir.filePath("optimized.bin"), &error);
        QVERIFY2(!text.isEmpty(), qPrintable(error));
        QCOMPARE(QStringList(binary.keys()).size(), QStringList(text.keys()).size());
        for (auto it = text.cbegin(); it != text.cend(); ++it) {
            const QVector<double> b = binary.value(it.key());
            QCOMPARE(b.size(), it.value().size());
            for (int i = 0; i < b.size(); ++i)
                QVERIFY2(b.at(i) == it.value().at(i) || (std::isnan(b.at(i)) && std::isnan(it.value().at(i))), qPrintable(it.key()));
        }
        QCOMPARE(binary.value("tran.v(out)").first(), 1.0);         // the last of its name
        QCOMPARE(binary.value("ac.v(out)").first(), std::hypot(0.5, 0.25));   // complex: its magnitude
        QCOMPARE(binary.value("tran.db(x)").first(), -52.0);        // imaginary parts 0: its real part
    }

    // The names a dataset's index gives: the dependent ones (Claude's hints
    // of traces, the Convert dialog's list).
    void theIndexNamesTheDependentVariables()
    {
        const QString path = dir.filePath("names.dat.ngspice");
        QSaveFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        df::BinaryWriter w(&f);
        sameBlocks(w);
        QVERIFY(w.finish(nullptr));
        QVERIFY(f.commit());
        QCOMPARE(df::dependentNames(path), (QStringList{"tran.v(out)", "ac.v(out)", "ac.mixed", "tran.db(x)", "tran.v(out)", "MC1.gain"}));
    }

    // A large run - wider than the file reads at once - binary, quick, its
    // raw file gone; each variable's every value where it belongs.
    void aLargeRunIsBinaryAndQuick()
    {
        const QString work = dir.filePath("large");
        QVERIFY(QDir().mkpath(work));
        const int vars = 1200, points = 3600;   // 34.6 MB
        QByteArray raw;
        {
            QStringList names{"time"};
            for (int v = 1; v < vars; ++v) names << QStringLiteral("v(n%1)").arg(v);
            raw = rawPlot("Transient Analysis", names, {}, points, false);
            QVector<double> row(vars);
            QByteArray data;
            data.reserve(qsizetype(vars) * points * 8);
            for (int p = 0; p < points; ++p) {
                for (int v = 0; v < vars; ++v) row[v] = v == 0 ? p * 1e-9 : v + p * 1e-6;
                data += doubles(row);
            }
            raw += data;
        }
        QVERIFY(!write(work + "/spice4qucs.tran.plot", raw).isEmpty());
        const bool binaryWas = QucsSettings.DatasetBinary;
        QucsSettings.DatasetBinary = true;
        QElapsedTimer timer;
        timer.start();
        const QString error = convert(work, {"spice4qucs.tran.plot"}, work + "/big.dat.ngspice", AbstractSpiceKernel::DatasetFormat::Settings);
        const qint64 ms = timer.elapsed();
        QucsSettings.DatasetBinary = binaryWas;
        QVERIFY2(error.isEmpty(), qPrintable(error));
        qInfo("%d vectors x %d points (%.1f MB) converted in %lld ms", vars, points, raw.size() / 1e6, ms);
        QVERIFY(df::isBinary(work + "/big.dat.ngspice"));
        QVERIFY(!QFileInfo::exists(work + "/spice4qucs.tran.plot"));
        QVERIFY2(QFileInfo(work + "/big.dat.ngspice").size() < raw.size() + 200000, "no larger than the raw file, but its index");
        df::BinaryReader r;
        QVERIFY(r.open(work + "/big.dat.ngspice"));
        QCOMPARE(r.blocks().size(), vars);
        for (int v : {0, 1, vars / 2, vars - 1}) {
            QVector<double> re;
            QVERIFY(r.values(r.blocks().at(v), &re, nullptr));
            QCOMPARE(re.size(), points);
            for (int p : {0, 1, points / 2, points - 1}) QCOMPARE(re.at(p), v == 0 ? p * 1e-9 : v + p * 1e-6);
        }
    }

    // A plain raw file into a binary dataset a few points at a time, the
    // last few fewer: each value where it belongs whatever the size of the
    // pieces - real and complex, a point at a time up to all at once.
    void aRawFileIsTakenInPieces()
    {
        const int vars = 5, points = 37;
        for (const bool complex : {false, true}) {
            QStringList names{"frequency"};
            for (int v = 1; v < vars; ++v) names << QStringLiteral("v(n%1)").arg(v);
            QVector<double> values;
            for (int p = 0; p < points; ++p)
                for (int v = 0; v < vars; ++v) {
                    values << p * 10.0 + v;
                    if (complex) values << -(p * 10.0 + v);
                }
            const QString rawPath = write(dir.filePath("pieces.plot"), rawPlot("AC Analysis", names, values, points, complex));
            df::RawHeader header;
            QVERIFY(df::readRawHeader(rawPath, &header));
            QVERIFY(df::isPlainBinaryRaw(header, QFileInfo(rawPath).size()));
            QCOMPARE(header.variables, names);
            QCOMPARE(header.points, qint64(points));
            for (const qint64 tile : {qint64(1), header.pointBytes() * 3, header.pointBytes() * 36, qint64(1) << 30}) {
                const QString path = dir.filePath("pieces.dat");
                QSaveFile f(path);
                QVERIFY(f.open(QIODevice::WriteOnly));
                df::BinaryWriter w(&f);
                QList<qint64> offsets;
                offsets << w.reserve(QStringLiteral("indep frequency %1").arg(points), points, false);
                for (int v = 1; v < vars; ++v) offsets << w.reserve(QStringLiteral("dep %1 frequency").arg(names.at(v)), points, complex);
                QVERIFY(df::transposeRaw(rawPath, header, w, offsets, nullptr, tile));
                QVERIFY(w.finish(nullptr));
                QVERIFY(f.commit());
                ds::Dataset d;
                QVERIFY(d.read(path));
                for (int v = 0; v < vars; ++v) {
                    const ds::Variable* var = d.find(names.at(v));
                    QVERIFY(var != nullptr);
                    QCOMPARE(var->isComplex(), complex && v > 0);
                    for (int p = 0; p < points; ++p) {
                        QCOMPARE(var->re.at(p), p * 10.0 + v);
                        if (var->isComplex()) QCOMPARE(var->im.at(p), -(p * 10.0 + v));
                    }
                }
            }
        }
        // Not plain: two plots, one cut short, one with dims=, an ASCII one.
        const QByteArray one = rawPlot("Transient Analysis", {"time", "v(a)"}, {0, 1, 2, 3}, 2, false);
        df::RawHeader h;
        QVERIFY(df::readRawHeader(write(dir.filePath("two.plot"), one + one), &h));
        QVERIFY(!df::isPlainBinaryRaw(h, QFileInfo(dir.filePath("two.plot")).size()));
        QVERIFY(df::readRawHeader(write(dir.filePath("short.plot"), one.chopped(3)), &h));
        QVERIFY(!df::isPlainBinaryRaw(h, QFileInfo(dir.filePath("short.plot")).size()));
        QVERIFY(df::readRawHeader(write(dir.filePath("dims.plot"), rawPlot("T", {"time", "v(d)"}, {0, 1, 2, 3}, 2, false, true, {"v(d)"})), &h));
        QVERIFY(h.dims && !df::isPlainBinaryRaw(h, QFileInfo(dir.filePath("dims.plot")).size()));
        QVERIFY(df::readRawHeader(write(dir.filePath("ascii.plot"), rawPlot("T", {"time", "v(a)"}, {0, 1, 2, 3}, 2, false, false)), &h));
        QVERIFY(!h.binary && !df::isPlainBinaryRaw(h, QFileInfo(dir.filePath("ascii.plot")).size()));
        QVERIFY(!df::readRawHeader(write(dir.filePath("none.plot"), "no header\n"), &h));
    }

    // QUCS_BENCH_DIR=<dir>: a run as large as the stress test's (12,503
    // vectors of 11,715 points, 1.17 GB) converted binary, and as text, in
    // a folder there; the times and the peak memory said.
    void benchmark()
    {
        const QString bench = qEnvironmentVariable("QUCS_BENCH_DIR");
        if (bench.isEmpty()) QSKIP("QUCS_BENCH_DIR names a folder for a 1.2 GB run");
        const int vars = qEnvironmentVariableIntValue("QUCS_BENCH_VARS") > 0 ? qEnvironmentVariableIntValue("QUCS_BENCH_VARS") : 12503;
        const int points = qEnvironmentVariableIntValue("QUCS_BENCH_POINTS") > 0 ? qEnvironmentVariableIntValue("QUCS_BENCH_POINTS") : 11715;
        const bool asText = qEnvironmentVariableIsSet("QUCS_BENCH_TEXT");
        QVERIFY(QDir().mkpath(bench));
        const QString rawPath = bench + "/spice4qucs.tran.plot";
        {
            QStringList names{"time"};
            for (int v = 1; v < vars; ++v) names << QStringLiteral("v(n%1)").arg(v);
            QFile f(rawPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(rawPlot("Transient Analysis", names, {}, points, false));
            QVector<double> row(vars);
            for (int p = 0; p < points; ++p) {
                for (int v = 0; v < vars; ++v) row[v] = v == 0 ? p * 1e-9 : std::sin(v + p * 1e-3);
                f.write(doubles(row));
            }
        }
        const qint64 rawSize = QFileInfo(rawPath).size();
        const double before = peakFootprintMB();
        QElapsedTimer timer;
        timer.start();
        const QString dataset = bench + (asText ? "/stress.text.dat.ngspice" : "/stress.dat.ngspice");
        const QString error = convert(bench, {"spice4qucs.tran.plot"}, dataset,
                                      asText ? AbstractSpiceKernel::DatasetFormat::Text : AbstractSpiceKernel::DatasetFormat::Binary);
        const qint64 convertMs = timer.elapsed();
        const double after = peakFootprintMB();
        QVERIFY2(error.isEmpty(), qPrintable(error));
        timer.restart();
        ds::Dataset d;
        QVERIFY(d.read(dataset));
        const qint64 readAllMs = timer.elapsed();
        timer.restart();
        RectDiagram diagram;
        auto* g = new Graph(&diagram, QStringLiteral("ngspice/tran.v(n%1)").arg(vars - 1));
        diagram.Graphs.append(g);
        QCOMPARE(g->loadDatFile(dataset.left(dataset.size() - 8)), 2);
        const qint64 graphMs = timer.elapsed();
        qInfo("%s: raw %.1f MB, dataset %.1f MB; converted in %lld ms, peak memory %.0f MB (%.0f MB before); "
              "read whole %lld ms; the last vector's graph %lld ms",
              asText ? "text" : "binary", rawSize / 1e6, QFileInfo(dataset).size() / 1e6, convertMs, after, before,
              readAllMs, graphMs);
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestBinaryDataset test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_binary_dataset.moc"
