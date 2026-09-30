/*
 * Data files of other programs imported as datasets (dataimport.h): tables
 * (CSV, TSV, text, Excel), NumPy's arrays, Touchstone files, datasets from
 * elsewhere - their columns, x, names; the dataset written beside a
 * schematic, where it came from kept in it, and a diagram's trace reading
 * it next to a simulation's.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "config.h"
#include "dataimport.h"
#include "dataset.h"
#include "spreadsheet.h"
#include "zipfile.h"
#include "diagrams/graph.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/diagramdialog.h"
#include "diagrams/dataimportpanel.h"
#include "main.h"
#include "schematic.h"
#include "extsimkernels/spicecompat.h"

#include <QComboBox>
#include <QLabel>
#include <QTableWidget>
#include <QTabWidget>
#include <QPainter>
#include <QElapsedTimer>
#include <QRandomGenerator>

#include <cmath>
#include <cstring>

namespace di = qucs_s::dataimport;
using qucs_s::dataset::Variable;

namespace {

// NumPy's .npy of \a descr and \a shape: its header as numpy writes it
// (padded to 64 bytes, ending in a newline), then \a data.
QByteArray npy(const QString& descr, const QString& shape, const QByteArray& data, bool fortran = false)
{
    QString header = QStringLiteral("{'descr': %1, 'fortran_order': %2, 'shape': %3, }")
                         .arg(descr, fortran ? QStringLiteral("True") : QStringLiteral("False"), shape);
    while ((10 + header.size() + 1) % 64 != 0) header += QLatin1Char(' ');
    header += QLatin1Char('\n');
    QByteArray out("\x93NUMPY\x01\x00", 8);
    out += char(header.size() & 0xff);
    out += char(header.size() >> 8);
    out += header.toLatin1();
    return out + data;
}

template <typename T>
QByteArray bytesOf(std::initializer_list<T> values, bool big = false)
{
    QByteArray out;
    for (T v : values) {
        char b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        if (big) std::reverse(b, b + sizeof(T));
        out.append(b, sizeof(T));
    }
    return out;
}

const Variable* find(const di::Data& d, const QString& name)
{
    for (const Variable& v : d.variables)
        if (v.name == name) return &v;
    return nullptr;
}

QStringList names(const di::Data& d)
{
    QStringList list;
    for (const Variable& v : d.variables) list << v.name;
    return list;
}

} // namespace

class TestDataImport : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString write(const QString& name, const QByteArray& bytes)
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        return path;
    }

    di::Data read(const QString& path, const di::Options& options = {})
    {
        di::Data d;
        QString error;
        if (!di::read(path, options, &d, &error)) qWarning() << path << error;
        return d;
    }

private slots:
    void initTestCase() { QVERIFY(dir.isValid()); }

    // A CSV file: comments before it, the line before the numbers names
    // the columns (a unit after them left out), a blank cell or N/A is no
    // value, a line of text among the numbers is left out (said). x is the
    // first column, which rises; the others are on it.
    void aCsvFileIsRead()
    {
        const QString path = write("measured.csv",
                                   "# Bench 3, 2026-09-30\n"
                                   "# instrument: scope\n"
                                   "Time (s),Voltage (V),I/out,Voltage (V)\n"
                                   "0,1.5,0.1,9\n"
                                   "1e-3,2.5,,9\n"
                                   "2e-3,3.5,N/A,9\n"
                                   "(paused)\n"
                                   "3e-3,4.5,0.4,9\n");
        const di::Data d = read(path);
        QCOMPARE(d.format, di::Format::Table);
        QCOMPARE(names(d), (QStringList{"Time", "Voltage", "I_out", "Voltage_2"}));
        QCOMPARE(d.x, QStringLiteral("Time"));
        const Variable* t = find(d, "Time");
        QVERIFY(t->independent);
        QCOMPARE(t->re, (QVector<double>{0, 1e-3, 2e-3, 3e-3}));
        const Variable* i = find(d, "I_out");
        QCOMPARE(i->dependencies, QStringList{"Time"});
        QCOMPARE(i->re.size(), 4);
        QVERIFY(std::isnan(i->re.at(1)) && std::isnan(i->re.at(2)));
        QCOMPARE(i->re.at(3), 0.4);
        QVERIFY2(d.notes.join(" ").contains("A line among the numbers that was not numbers was left out."), qPrintable(d.notes.join(" | ")));
        QCOMPARE(d.columns, (QStringList{"Time", "Voltage", "I_out", "Voltage_2"}));
    }

    // With ; between the columns, a decimal comma; TSV; a column of text
    // left out.
    void semicolonsTabsAndText()
    {
        di::Data d = read(write("euro.csv", "f;gain;label\n1,0;2,5;a\n2,0;3,5;b\n"));
        QCOMPARE(names(d), (QStringList{"f", "gain"}));
        QCOMPARE(find(d, "gain")->re, (QVector<double>{2.5, 3.5}));
        QVERIFY(d.notes.join(" ").contains("label has no numbers"));
        d = read(write("tabs.tsv", "x\ty\n1\t10\n2\t20\n3\t30\n"));
        QCOMPARE(names(d), (QStringList{"x", "y"}));
        QCOMPARE(find(d, "y")->re, (QVector<double>{10, 20, 30}));
    }

    // Text in columns apart by spaces: a comment names them (numpy's
    // savetxt header); a count before the numbers is no name; no names:
    // A, B, C.
    void textInColumns()
    {
        di::Data d = read(write("wave.txt", "% from a script\n1001\n# time v(out) i(r1)\n0 0.0 1e-3\n1e-6 0.5 2e-3\n2e-6 1.0 3e-3\n"));
        QCOMPARE(d.format, di::Format::Text);
        QCOMPARE(names(d), (QStringList{"time", "v(out)", "i(r1)"}));
        QCOMPARE(find(d, "v(out)")->re, (QVector<double>{0, 0.5, 1.0}));
        d = read(write("plain.prn", "  1   5.5  \n  2   6.5\n  3   7.5\n"));
        QCOMPARE(names(d), (QStringList{"A", "B"}));
        QCOMPARE(d.x, QStringLiteral("A"));
        d = read(write("counted.txt", "3\n0 1\n1 2\n2 3\n"));   // (a count right before the numbers: no names)
        QCOMPARE(names(d), (QStringList{"A", "B"}));
        d = read(write("tabs.txt", "Time (s)\tV out\n0\t1\n1\t2\n"));   // (tabs, names with spaces)
        QCOMPARE(names(d), (QStringList{"Time", "V_out"}));
        d = read(write("ones.data", "7\n8\n9\n"));
        QCOMPARE(names(d), (QStringList{"row", "A"}));   // (one column: over the row)
        QCOMPARE(find(d, "row")->re, (QVector<double>{1, 2, 3}));
    }

    // x: the first column when it rises or falls steadily, else the row;
    // one chosen by name; the row when asked; a name there is not: said,
    // and as by default. Rows with no value of x are left out.
    void theXIsChosen()
    {
        const QString path = write("xs.csv", "a,b,c\n3,1,5\n2,2,6\n,3,7\n1,4,8\n");
        di::Data d = read(path);
        QCOMPARE(d.x, QStringLiteral("row"));   // (a has a gap: not steady)
        QCOMPARE(find(d, "row")->re, (QVector<double>{1, 2, 3, 4}));
        d = read(path, {QString(), "b"});
        QCOMPARE(d.x, QStringLiteral("b"));
        QCOMPARE(names(d), (QStringList{"b", "a", "c"}));
        d = read(path, {QString(), "a"});
        QCOMPARE(find(d, "a")->re, (QVector<double>{3, 2, 1}));
        QCOMPARE(find(d, "c")->re, (QVector<double>{5, 6, 8}));   // (the row with no a left out)
        QVERIFY(d.notes.join(" ").contains("A row with no value of x was left out."));
        d = read(path, {QString(), di::rowX()});
        QCOMPARE(d.x, QStringLiteral("row"));
        d = read(path, {QString(), "nosuch"});
        QVERIFY(d.notes.join(" ").contains("There is no column nosuch"));
        d = read(write("falls.csv", "v,i\n5,1\n4,2\n3,3\n"));
        QCOMPARE(d.x, QStringLiteral("v"));   // (falling steadily)
    }

    void namesAreMadeSafe()
    {
        QCOMPARE(di::safeName("Voltage (V)"), QStringLiteral("Voltage"));
        QCOMPARE(di::safeName("Freq [Hz]"), QStringLiteral("Freq"));
        QCOMPARE(di::safeName("v(out)"), QStringLiteral("v(out)"));
        QCOMPARE(di::safeName("S[2,1]"), QStringLiteral("S[2_1]"));
        QCOMPARE(di::safeName("a/b:c@d"), QStringLiteral("a_b_c_d"));
        QCOMPARE(di::safeName("_hidden"), QStringLiteral("hidden"));
        QCOMPARE(di::safeName("1st"), QStringLiteral("c1st"));
        QCOMPARE(di::safeName("  "), QStringLiteral("c"));
        QCOMPARE(di::safeName("I-out dB"), QStringLiteral("I_out_dB"));
    }

    // An Excel workbook: a sheet chosen by name, the first by default.
    void aWorkbookIsRead()
    {
        namespace sh = qucs_s::sheet;
        sh::Workbook book;
        book.format = sh::Format::Xlsx;
        for (const char* name : {"Summary", "Sweep"}) {
            sh::Sheet s;
            s.name = QString::fromLatin1(name);
            book.sheets << s;
        }
        sh::enter(book.sheets[0].cell(0, 0), "Only text here", book);
        sh::enter(book.sheets[1].cell(0, 0), "freq", book);
        sh::enter(book.sheets[1].cell(0, 1), "gain", book);
        for (int r = 1; r <= 3; ++r) {
            sh::enter(book.sheets[1].cell(r, 0), QString::number(r * 1000), book);
            sh::enter(book.sheets[1].cell(r, 1), QString::number(r * 0.5), book);
        }
        const QString path = dir.filePath("book.xlsx");
        QString error;
        QVERIFY2(sh::writeFile(path, book, 0, &error), qPrintable(error));
        di::Data d;
        QVERIFY(!di::read(path, {}, &d, &error));   // (the first sheet has no numbers)
        QVERIFY2(error.contains("no rows of numbers"), qPrintable(error));
        d = read(path, {"Sweep", QString()});
        QCOMPARE(d.format, di::Format::Workbook);
        QCOMPARE(d.sheets, (QStringList{"Summary", "Sweep"}));
        QCOMPARE(names(d), (QStringList{"freq", "gain"}));
        QCOMPARE(find(d, "gain")->re, (QVector<double>{0.5, 1.0, 1.5}));
    }

    // NumPy: two dimensions (in C's order and Fortran's), whole numbers,
    // complex ones, named fields, big-endian, true and false; an archive of
    // them by name, one of another length left out. Refused: Python
    // objects, three dimensions, data cut short.
    void numpyArraysAreRead()
    {
        di::Data d = read(write("grid.npy", npy("'<f8'", "(3, 2)", bytesOf<double>({0, 1, 1, 2, 2, 4}))));
        QCOMPARE(d.format, di::Format::Npy);
        QCOMPARE(names(d), (QStringList{"grid_0", "grid_1"}));
        QCOMPARE(find(d, "grid_1")->re, (QVector<double>{1, 2, 4}));
        d = read(write("fgrid.npy", npy("'<f8'", "(3, 2)", bytesOf<double>({0, 1, 2, 1, 2, 4}), true)));
        QCOMPARE(find(d, "fgrid_1")->re, (QVector<double>{1, 2, 4}));
        d = read(write("counts.npy", npy("'<i4'", "(3,)", bytesOf<qint32>({7, -8, 9}))));
        QCOMPARE(names(d), (QStringList{"row", "counts"}));
        QCOMPARE(find(d, "counts")->re, (QVector<double>{7, -8, 9}));
        d = read(write("z.npy", npy("'<c16'", "(2,)", bytesOf<double>({1, 2, 3, -4}))));
        QCOMPARE(find(d, "z")->re, (QVector<double>{1, 3}));
        QCOMPARE(find(d, "z")->im, (QVector<double>{2, -4}));
        QByteArray rec;
        rec += bytesOf<double>({0.0}) + bytesOf<float>({1.5f}) + bytesOf<double>({1.0}) + bytesOf<float>({2.5f});
        d = read(write("rec.npy", npy("[('t', '<f8'), ('v', '<f4')]", "(2,)", rec)));
        QCOMPARE(names(d), (QStringList{"t", "v"}));
        QCOMPARE(find(d, "v")->re, (QVector<double>{1.5, 2.5}));
        d = read(write("big.npy", npy("'>f8'", "(2,)", bytesOf<double>({1.25, 2.5}, true))));
        QCOMPARE(find(d, "big")->re, (QVector<double>{1.25, 2.5}));
        d = read(write("flags.npy", npy("'|b1'", "(3,)", QByteArray("\x01\x00\x01", 3))));
        QCOMPARE(find(d, "flags")->re, (QVector<double>{1, 0, 1}));
        QString error;
        for (const auto& [file, bytes, why] :
             {std::tuple("obj.npy", npy("'|O'", "(2,)", QByteArray(16, 0)), "Python objects"),
              std::tuple("cube.npy", npy("'<f8'", "(2, 2, 2)", QByteArray(64, 0)), "3 dimensions"),
              std::tuple("short.npy", npy("'<f8'", "(100,)", QByteArray(16, 0)), "cut short")}) {
            QVERIFY2(!di::read(write(file, bytes), {}, &d, &error) && error.contains(why), qPrintable(file + (": " + error)));
        }
        // An archive: its arrays by name; one of another length left out.
        const QByteArray archive = qucs_s::zip::write(
            {{"f.npy", npy("'<f8'", "(3,)", bytesOf<double>({1e3, 2e3, 3e3}))},
             {"mag.npy", npy("'<f4'", "(3,)", bytesOf<float>({0.5f, 0.25f, 0.125f}))},
             {"iq.npy", npy("'<c8'", "(3,)", bytesOf<float>({1, 1, 2, 2, 3, 3}))},
             {"meta.npy", npy("'<i8'", "(2,)", bytesOf<qint64>({1, 2}))}});
        d = read(write("sweep.npz", archive));
        QCOMPARE(d.format, di::Format::Npz);
        QCOMPARE(names(d), (QStringList{"f", "mag", "iq"}));
        QCOMPARE(d.x, QStringLiteral("f"));
        QCOMPARE(find(d, "iq")->im, (QVector<double>{1, 2, 3}));
        QVERIFY(d.notes.join(" ").contains("meta has 2 values, not the 3 of the others"));
    }

    // Touchstone: one port in MA and GHz; two in RI, version 1's order (11
    // 21 12 22), noise data after them left out; version 2 in its order;
    // three ports in dB, their rows over lines.
    void touchstoneIsRead()
    {
        di::Data d = read(write("ant.s1p", "! an antenna\n# GHz S MA R 50\n1 0.5 90\n2 0.25 180\n"));
        QCOMPARE(d.format, di::Format::Touchstone);
        QCOMPARE(names(d), (QStringList{"frequency", "S[1,1]"}));
        QCOMPARE(find(d, "frequency")->re, (QVector<double>{1e9, 2e9}));
        const Variable* s11 = find(d, "S[1,1]");
        QVERIFY(std::abs(s11->re.at(0)) < 1e-12 && std::abs(s11->im.at(0) - 0.5) < 1e-12);
        QVERIFY(std::abs(s11->re.at(1) + 0.25) < 1e-12);
        d = read(write("amp.s2p", "# MHz S RI R 50\n"
                                  "100 0.1 0 2 0 0.01 0 0.2 0\n"
                                  "200 0.1 0 3 0 0.01 0 0.2 0\n"
                                  "! noise\n"
                                  "100 1.5 0.2 30 0.4\n"
                                  "200 1.6 0.2 31 0.4\n"));
        QCOMPARE(find(d, "frequency")->re, (QVector<double>{1e8, 2e8}));
        QCOMPARE(find(d, "S[2,1]")->re, (QVector<double>{2, 3}));
        QCOMPARE(find(d, "S[1,2]")->re, (QVector<double>{0.01, 0.01}));
        d = read(write("v2.s2p", "[Version] 2.0\n# Hz Y RI R 50\n[Number of Ports] 2\n[Two-Port Data Order] 12_21\n"
                                 "[Number of Frequencies] 1\n[Network Data]\n1 11 0 12 0 21 0 22 0\n[End]\n"));
        QCOMPARE(find(d, "Y[1,2]")->re.first(), 12.0);
        QCOMPARE(find(d, "Y[2,1]")->re.first(), 21.0);
        d = read(write("tri.s3p", "# Hz S DB R 50\n1 0 0 -20 0 -20 0\n  -20 0 0 0 -20 0\n  -20 0 -20 0 -6.0206 0\n"));
        QCOMPARE(d.variables.size(), 10);
        QVERIFY(std::abs(find(d, "S[1,2]")->re.first() - 0.1) < 1e-9);
        QVERIFY(std::abs(find(d, "S[3,3]")->re.first() - 0.5) < 1e-4);
    }

    // Written beside a schematic: not over its simulation's dataset nor any
    // other (amp.csv beside amp.sch is amp_2), the same source read again
    // into its own; where it came from kept in it, which the datasets'
    // readers pass by; complex values and no value kept; listed; and a
    // diagram's trace names it next to the simulation's dataset.
    void aDatasetIsWrittenBesideTheSchematic()
    {
        QDir(dir.path()).mkpath("proj");
        const QString folder = dir.filePath("proj");
        write("proj/amp.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n");
        write("proj/amp.dat.ngspice", "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 2>\n0\n1\n</indep>\n<dep tran.v(out) time>\n0\n1\n</dep>\n");
        const QString source = write("amp.csv", "f,v\n1,0.5\n2,\n3,1.5\n");
        di::Imported one;
        QString error;
        QStringList notes;
        QVERIFY2(di::importFile(folder, source, {}, &one, &error, &notes), qPrintable(error));
        QCOMPARE(one.name, QStringLiteral("amp_2"));
        QCOMPARE(one.origin.source, QFileInfo(source).absoluteFilePath());
        QCOMPARE(one.origin.format, QStringLiteral("CSV"));
        QCOMPARE(one.origin.variables, 2);
        QCOMPARE(one.origin.points, 3);
        di::Imported again;
        QVERIFY(di::importFile(folder, source, {QString(), di::rowX()}, &again, &error));
        QCOMPARE(again.name, QStringLiteral("amp_2"));   // (read again into its own)
        QCOMPARE(again.origin.options.x, di::rowX());
        QVERIFY(di::importFile(folder, source, {}, &again, &error));
        di::Imported second;
        QVERIFY(QDir(dir.path()).mkpath("sub"));
        QVERIFY(di::importFile(folder, write("sub/amp.csv", "f,v\n1,1\n2,2\n"), {}, &second, &error));
        QCOMPARE(second.name, QStringLiteral("amp_3"));   // (another file of the same name)
        // A schematic whose simulation has not run yet: its name kept for it.
        write("proj/filt.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n");
        di::Imported filt;
        QVERIFY(di::importFile(folder, write("filt.csv", "f,g\n1,2\n2,3\n"), {}, &filt, &error));
        QCOMPARE(filt.name, QStringLiteral("filt_2"));
        QVERIFY(QFile::remove(filt.path));
        const QList<di::Imported> listed = di::importedIn(folder);
        QCOMPARE(listed.size(), 2);
        QCOMPARE(listed.first().name, QStringLiteral("amp_2"));
        di::Origin none;
        QVERIFY(!di::originOf(dir.filePath("proj/amp.dat.ngspice"), &none));

        // Read back as any dataset is: values, no value, complex.
        qucs_s::dataset::Dataset back;
        QVERIFY2(back.read(one.path, &error), qPrintable(error));
        QCOMPARE(back.find("f")->re, (QVector<double>{1, 2, 3}));
        QVERIFY(std::isnan(back.find("v")->re.at(1)));
        QCOMPARE(back.find("v")->dependencies, QStringList{"f"});
        di::Data complex;
        complex.variables = {Variable{"x", true, {}, {1, 2}, {}}, Variable{"z", false, {"x"}, {1, -2}, {-0.5, 0.0}}};
        QVERIFY(di::writeDataset(dir.filePath("proj/cplx.dat"), complex, {}, &error));
        QVERIFY(back.read(dir.filePath("proj/cplx.dat"), &error));
        QCOMPARE(back.find("z")->re, (QVector<double>{1, -2}));
        QCOMPARE(back.find("z")->im, (QVector<double>{-0.5, 0.0}));

        // A trace of it, with the schematic's dataset the default.
        QString body = QStringLiteral("<\"amp_2:v\" #0000ff 0 3 0 0 0>\n<\"ngspice/tran.v(out)\" #ff0000 0 3 0 0 0>\n</Rect>\n");
        QTextStream stream(&body, QIODevice::ReadOnly);
        RectDiagram d;
        QVERIFY(d.load("<Rect 0 300 600 300 3 #c0c0c0 1 00 1 0 0.2 1 0 0 0.5 1 1 -0.1 0.5 1.1 315 0 225 1 0 0 \"\" \"\" \"\">", &stream));
        d.loadGraphData(dir.filePath("proj/amp.dat"));
        QCOMPARE(d.Graphs.size(), size_t(2));
        QCOMPARE(d.Graphs.at(0)->count(0), size_t(3));
        QCOMPARE(d.Graphs.at(0)->axisName(0), QStringLiteral("f"));
        QCOMPARE(d.Graphs.at(1)->count(0), size_t(2));
    }

    // A dataset from elsewhere: its variables as they are, x its first
    // independent one.
    void aDatasetFromElsewhere()
    {
        const QString path = write("old.dat", "<Qucs Dataset 0.0.19>\n<indep freq 2>\n  1e3\n  2e3\n</indep>\n"
                                              "<dep S21 freq>\n  +1.0e+00+j2.0e+00\n  +3.0e+00-j4.0e+00\n</dep>\n");
        const di::Data d = read(path);
        QCOMPARE(d.format, di::Format::Dataset);
        QCOMPARE(names(d), (QStringList{"freq", "S21"}));
        QCOMPARE(d.x, QStringLiteral("freq"));
        QCOMPARE(find(d, "S21")->im, (QVector<double>{2, -4}));
        QVERIFY(d.columns.isEmpty());   // (x is its own)
    }

    // QUCS_IMPORT_SAMPLES=<dir>: files other programs wrote (NumPy, pandas),
    // each with <file>.expect.json - {"x": name, "variables": {name: {"re":
    // [...], "im": [...]}}} - read and compared.
    void samplesMatchTheirWriters()
    {
        const QString samples = qEnvironmentVariable("QUCS_IMPORT_SAMPLES");
        if (samples.isEmpty()) QSKIP("set QUCS_IMPORT_SAMPLES to a folder of samples");
        int files = 0;
        for (const QFileInfo& expect : QDir(samples).entryInfoList({"*.expect.json"}, QDir::Files, QDir::Name)) {
            QFile f(expect.filePath());
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QJsonObject want = QJsonDocument::fromJson(f.readAll()).object();
            const QString path = QDir(samples).filePath(expect.fileName().chopped(QStringLiteral(".expect.json").size()));
            di::Data d;
            QString error;
            QVERIFY2(di::read(path, {QString(), want.value("options_x").toString()}, &d, &error), qPrintable(path + ": " + error));
            QVERIFY2(d.x == want.value("x").toString(), qPrintable(path + ": x " + d.x));
            const QJsonObject vars = want.value("variables").toObject();
            for (auto it = vars.begin(); it != vars.end(); ++it) {
                const Variable* v = find(d, it.key());
                QVERIFY2(v != nullptr, qPrintable(path + ": no " + it.key() + " in " + names(d).join(",")));
                const QJsonArray re = it.value().toObject().value("re").toArray(), im = it.value().toObject().value("im").toArray();
                QVERIFY2(v->re.size() == re.size(), qPrintable(path + ": " + it.key()));
                for (int i = 0; i < re.size(); ++i) {
                    const double a = v->re.at(i), b = re.at(i).toDouble();
                    QVERIFY2(std::abs(a - b) <= 1e-6 * std::max(1.0, std::abs(b)),
                             qPrintable(QStringLiteral("%1: %2[%3] %4 != %5").arg(path, it.key()).arg(i).arg(a).arg(b)));
                }
                QVERIFY2(im.isEmpty() == !v->isComplex(), qPrintable(path + ": " + it.key() + " complex?"));
                for (int i = 0; i < im.size(); ++i)
                    QVERIFY2(std::abs(v->im.at(i) - im.at(i).toDouble()) <= 1e-6 * std::max(1.0, std::abs(im.at(i).toDouble())),
                             qPrintable(path));
            }
            ++files;
        }
        QVERIFY(files > 0);
        qInfo() << files << "samples as their writers wrote them";
    }

    // The Import tab, right of Theme: files added - several at once - are
    // datasets the Data tab lists, said to be imported, next to the
    // simulation's; a variable taken from one is a trace name:variable,
    // one of the simulation's ngspice/variable, and the diagram draws both.
    // x chosen: read again. The file changed: read again. Removed: gone
    // from the list. A schematic not saved: nothing to import into.
    void theImportTabFeedsTheDataTab()
    {
        QDir(dir.path()).mkpath("tab");
        const QString folder = QFileInfo(dir.filePath("tab")).canonicalFilePath();
        write("tab/amp.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n");
        write("tab/amp.dat.ngspice", "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 3>\n0\n1e-3\n2e-3\n</indep>\n"
                                     "<dep tran.v(out) time>\n0\n0.5\n1\n</dep>\n");
        const QString measured = write("bench.csv", "time,vout,iout\n0,0.1,1\n1e-3,0.6,2\n2e-3,1.1,3\n3e-3,1.5,4\n");
        const QString wave = write("wave.npy", npy("'<f8'", "(2, 2)", bytesOf<double>({0, 5, 1, 6})));
        const int simulator = QucsSettings.DefaultSimulator;
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        {
            Schematic sch(nullptr, folder + "/amp.sch");
            RectDiagram d;
            auto* dialog = new DiagramDialog(&d, &sch);
            auto* tabs = dialog->findChild<QTabWidget*>();
            QStringList titles;
            for (int i = 0; i < tabs->count(); ++i) titles << tabs->tabText(i);
            QCOMPARE(titles, (QStringList{"Data", "Properties", "Limits", "Theme", "Import"}));
            auto* panel = dialog->findChild<DataImportPanel*>();
            QVERIFY(panel != nullptr && panel->addButton()->isEnabled());
            QCOMPARE(panel->folder(), folder);
            auto* datasets = dialog->findChild<QComboBox*>("diagramDataset");
            auto* simulators = dialog->findChild<QComboBox*>("diagramSimulator");
            auto* variables = dialog->findChild<QTableWidget*>("diagramVariables");
            const auto listed = [&] {
                QStringList texts;
                for (int i = 0; i < datasets->count(); ++i) texts << datasets->itemText(i);
                return texts;
            };
            const auto names = [&] {
                QStringList list;
                for (int r = 0; r < variables->rowCount(); ++r)
                    if (variables->item(r, 0) != nullptr) list << variables->item(r, 0)->text();
                list.sort();
                return list;
            };
            QCOMPARE(listed(), QStringList{"amp"});
            QCOMPARE(simulators->currentText(), QStringLiteral("Ngspice"));

            // Both at once: the last shown.
            panel->importFiles({measured, wave});
            QCOMPARE(panel->list()->rowCount(), 2);
            QVERIFY2(panel->status()->text().contains("bench.csv is the dataset bench: 3 variables, 4 points."),
                     qPrintable(panel->status()->text()));
            QCOMPARE(listed(), (QStringList{"amp", "bench  (imported)", "wave  (imported)"}));
            QCOMPARE(datasets->currentData().toString(), QStringLiteral("wave"));
            QCOMPARE(simulators->currentText(), QStringLiteral("wave.npy"));
            QCOMPARE(names(), (QStringList{"wave_0", "wave_1"}));

            // A variable of each: bench's, then the simulation's.
            datasets->setCurrentIndex(datasets->findData("bench"));
            QMetaObject::invokeMethod(dialog, "slotReadVarsAndSetSimulator", Q_ARG(int, 0));
            QCOMPARE(names(), (QStringList{"iout", "time", "vout"}));
            for (int r = 0; r < variables->rowCount(); ++r)
                if (variables->item(r, 0)->text() == "vout") emit variables->itemDoubleClicked(variables->item(r, 0));
            datasets->setCurrentIndex(datasets->findData("amp"));
            QMetaObject::invokeMethod(dialog, "slotReadVarsAndSetSimulator", Q_ARG(int, 0));
            QCOMPARE(simulators->currentText(), QStringLiteral("Ngspice"));
            QCOMPARE(names(), (QStringList{"time", "tran.v(out)"}));
            for (int r = 0; r < variables->rowCount(); ++r)
                if (variables->item(r, 0)->text() == "tran.v(out)") emit variables->itemDoubleClicked(variables->item(r, 0));
            auto* graphs = dialog->findChild<QTableWidget*>("diagramGraphs");
            QCOMPARE(graphs->rowCount(), 2);
            QCOMPARE(graphs->item(0, 0)->text(), QStringLiteral("bench:vout"));
            QCOMPARE(graphs->item(1, 0)->text(), QStringLiteral("ngspice/tran.v(out)"));

            // x chosen: read again, and listed so.
            panel->setOptions("bench", {QString(), di::rowX()});
            QVERIFY(panel->status()->text().contains("bench read again, x the row"));
            QCOMPARE(datasets->currentData().toString(), QStringLiteral("bench"));
            QCOMPARE(names(), (QStringList{"iout", "row", "time", "vout"}));
            panel->setOptions("bench", {});
            // The file changed: read again.
            write("bench.csv", "time,vout,iout\n0,0.1,1\n1e-3,0.6,2\n2e-3,1.1,3\n3e-3,1.5,4\n4e-3,1.7,5\n");
            panel->reload({"bench"});
            QVERIFY2(panel->status()->text().contains("bench read again: 3 variables, 5 points."), qPrintable(panel->status()->text()));
            // Removed: gone, and its file.
            panel->remove({"wave"});
            QCOMPARE(listed(), (QStringList{"amp", "bench  (imported)"}));
            QVERIFY(!QFileInfo::exists(folder + "/wave.dat"));
            QVERIFY(QFileInfo::exists(wave));   // (the file it came from stays)

            // OK: the diagram draws the two, imported and simulated.
            QMetaObject::invokeMethod(dialog, "slotOK");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCOMPARE(d.Graphs.size(), size_t(2));
            d.loadGraphData(folder + "/amp.dat");
            QCOMPARE(d.Graphs.at(0)->count(0), size_t(5));
            QCOMPARE(d.Graphs.at(1)->count(0), size_t(3));
        }
        // Not saved: no folder, nothing to import into.
        {
            Schematic untitled(nullptr, QString());
            RectDiagram d;
            auto* dialog = new DiagramDialog(&d, &untitled);
            auto* panel = dialog->findChild<DataImportPanel*>();
            QVERIFY(panel->folder().isEmpty() && !panel->addButton()->isEnabled());
            dialog->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        QucsSettings.DefaultSimulator = simulator;
    }

    // QUCS_TEST_GRAB=<dir>: pictures of the Import tab, the Data tab and a
    // diagram with a measured curve over the simulated one.
    void pictures()
    {
        const QString grabs = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (grabs.isEmpty()) QSKIP("set QUCS_TEST_GRAB to a folder for the pictures");
        QDir(dir.path()).mkpath("pics");
        const QString folder = QFileInfo(dir.filePath("pics")).canonicalFilePath();
        write("pics/rc.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n");
        QByteArray sim = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 101>\n";
        for (int i = 0; i <= 100; ++i) sim += QByteArray::number(i * 1e-5) + "\n";
        sim += "</indep>\n<dep tran.v(out) time>\n";
        for (int i = 0; i <= 100; ++i) sim += QByteArray::number(1 - std::exp(-i * 1e-5 / 2e-4)) + "\n";
        sim += "</dep>\n";
        write("pics/rc.dat.ngspice", sim);
        QByteArray csv = "# scope capture, channel 2\nTime (s),Vout (V)\n";
        for (int i = 0; i <= 40; ++i) {
            const double t = i * 2.5e-5;
            csv += QByteArray::number(t) + "," + QByteArray::number(1 - std::exp(-t / 2.2e-4) + 0.02 * std::sin(i * 7.0)) + "\n";
        }
        const QString measured = write("scope.csv", csv);
        const QString sweep = write("sweep.npz", qucs_s::zip::write({{"f.npy", npy("'<f8'", "(3,)", bytesOf<double>({1e3, 1e4, 1e5}))},
                                                                   {"gain.npy", npy("'<f8'", "(3,)", bytesOf<double>({1, 0.7, 0.1}))}}));
        const int simulator = QucsSettings.DefaultSimulator;
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        {
            Schematic sch(nullptr, folder + "/rc.sch");
            RectDiagram d;
            auto* dialog = new DiagramDialog(&d, &sch);
            dialog->resize(820, 560);
            dialog->show();
            auto* tabs = dialog->findChild<QTabWidget*>();
            auto* panel = dialog->findChild<DataImportPanel*>();
            panel->importFiles({measured, sweep});
            panel->list()->selectRow(0);
            tabs->setCurrentWidget(panel);
            QTest::qWait(50);
            dialog->grab().save(grabs + "/import-tab.png");
            auto* datasets = dialog->findChild<QComboBox*>("diagramDataset");
            auto* variables = dialog->findChild<QTableWidget*>("diagramVariables");
            const auto take = [&](const QString& dataset, const QString& name) {
                datasets->setCurrentIndex(datasets->findData(dataset));
                QMetaObject::invokeMethod(dialog, "slotReadVarsAndSetSimulator", Q_ARG(int, 0));
                for (int r = 0; r < variables->rowCount(); ++r)
                    if (variables->item(r, 0)->text() == name) emit variables->itemDoubleClicked(variables->item(r, 0));
            };
            take("rc", "tran.v(out)");
            take("scope", "Vout");
            tabs->setCurrentIndex(0);
            QTest::qWait(50);
            dialog->grab().save(grabs + "/data-tab.png");
            QMetaObject::invokeMethod(dialog, "slotOK");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            d.x2 = 480;
            d.y2 = 300;
            d.Graphs.at(1)->Style = GRAPHSTYLE_STAR;
            d.loadGraphData(folder + "/rc.dat");
            QImage img(d.x2 + 120, d.y2 + 80, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(60, d.y2 + 20);
            d.cx = 0;
            d.cy = 0;
            d.paintDiagram(&p);
            p.end();
            img.save(grabs + "/diagram.png");
        }
        QucsSettings.DefaultSimulator = simulator;
    }

    // QUCS_IMPORT_FUZZ=<seconds>: seeds of every format mutated - bytes
    // changed, cut, repeated, huge counts - and read, for the sanitizers
    // (QUCS_IMPORT_SAMPLES adds its files to the seeds).
    void fuzz()
    {
        const int seconds = qEnvironmentVariableIntValue("QUCS_IMPORT_FUZZ");
        if (seconds <= 0) QSKIP("set QUCS_IMPORT_FUZZ to a number of seconds");
        QList<QPair<QString, QByteArray>> seeds{
            {"a.csv", "# c\nTime (s),V,I\n0,1,2\n1,,3\n2,N/A,4\n"},
            {"b.txt", "% x\n5\n# t v\n0 1\n1 2\n2 3\n"},
            {"c.tsv", "x\ty\n1\t2\n"},
            {"d.csv", "f;g\n1,5;2,5\n"},
            {"e.npy", npy("'<f8'", "(3, 2)", bytesOf<double>({0, 1, 1, 2, 2, 4}))},
            {"f.npy", npy("[('t', '<f8'), ('v', '<c8')]", "(1,)", bytesOf<double>({1.0}) + bytesOf<float>({1, 2}))},
            {"g.npz", qucs_s::zip::write({{"x.npy", npy("'<i2'", "(2,)", bytesOf<qint16>({1, 2}))},
                                          {"y.npy", npy("'>f4'", "(2,)", bytesOf<float>({1, 2}, true))}})},
            {"h.s2p", "# MHz S RI R 50\n100 0.1 0 2 0 0.01 0 0.2 0\n200 0.1 0 3 0 0.01 0 0.2 0\n"},
            {"i.s1p", "[Version] 2.0\n# GHz S DB R 50\n[Number of Ports] 1\n[Network Data]\n1 -3 45\n[End]\n"},
            {"j.dat", "<Qucs Dataset 1>\n<indep x 2>\n1\n2\n</indep>\n<dep y x>\n1+j2\n3-j4\n</dep>\n"},
        };
        const QString samples = qEnvironmentVariable("QUCS_IMPORT_SAMPLES");
        if (!samples.isEmpty())
            for (const QFileInfo& info : QDir(samples).entryInfoList(QDir::Files))
                if (!info.fileName().endsWith(".json")) {
                    QFile f(info.filePath());
                    if (f.open(QIODevice::ReadOnly)) seeds << qMakePair(info.fileName(), f.readAll());
                }
        QRandomGenerator rng(quint32(qEnvironmentVariableIntValue("QUCS_IMPORT_FUZZ_SEED") + 1));
        QElapsedTimer clock;
        clock.start();
        int runs = 0;
        while (clock.elapsed() < seconds * 1000) {
            const auto& [name, seed] = seeds.at(int(rng.bounded(quint32(seeds.size()))));
            QByteArray b = seed;
            for (int k = int(rng.bounded(1, 6)); k > 0 && !b.isEmpty(); --k) {
                const int at = int(rng.bounded(quint32(b.size())));
                switch (rng.bounded(7)) {
                case 0: b[at] = char(rng.bounded(256)); break;
                case 1: b.remove(at, int(rng.bounded(1, 16))); break;
                case 2: b.insert(at, b.mid(int(rng.bounded(quint32(b.size()))), int(rng.bounded(1, 64)))); break;
                case 3: b.truncate(at); break;
                case 4: b.insert(at, QByteArray::number(qint64(rng.generate64() >> rng.bounded(64)))); break;
                case 5: b.insert(at, QByteArray("\n,;\t #!<>()[]'\"j-+eE.0123456789").mid(int(rng.bounded(30)), 1)); break;
                default: b.insert(at, QByteArray(int(rng.bounded(1, 8)), char(rng.bounded(256)))); break;
                }
            }
            const QString path = write(QStringLiteral("fz_%1_%2").arg(runs % 8).arg(name), b);
            di::Data d;
            QString error;
            if (di::read(path, {QString(), rng.bounded(3) == 0 ? di::rowX() : QString()}, &d, &error)) {
                // Written and read back as a dataset, as the Import tab does.
                di::writeDataset(dir.filePath("fz.dat"), d, {}, &error);
                qucs_s::dataset::Dataset back;
                back.read(dir.filePath("fz.dat"), &error);
            }
            QFile::remove(path);
            ++runs;
        }
        qInfo() << runs << "files read";
    }

    // What does not read: said.
    void whatDoesNotRead()
    {
        di::Data d;
        QString error;
        QVERIFY(!di::read(dir.filePath("nowhere.csv"), {}, &d, &error));
        QVERIFY(error.contains("cannot be read"));
        QVERIFY(!di::read(write("words.txt", "just some words\nand more\n"), {}, &d, &error));
        QVERIFY2(error.contains("no rows of numbers"), qPrintable(error));
        QVERIFY(!di::read(write("blob.bin", QByteArray("\x01\x02\x00\x03", 4)), {}, &d, &error));
        QVERIFY2(error.contains("not a data file"), qPrintable(error));
        QVERIFY(!di::read(write("ports.snp", "# GHz S MA R 50\n1 0.5 0\n"), {}, &d, &error));
        QVERIFY2(error.contains("ports"), qPrintable(error));
    }
};

QTEST_MAIN(TestDataImport)
#include "test_data_import.moc"
