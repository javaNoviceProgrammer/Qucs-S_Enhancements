/*
 * test_data_export.cpp - a dataset's variables written for other programs
 * (dataexport.h), and the Export tab of a diagram's dialog
 *
 * This file is part of Qucs-S.
 */
#include <QtTest>
#include <QTemporaryDir>

#include "config.h"
#include "dataexport.h"
#include "dataimport.h"
#include "dataset.h"
#include "spreadsheet.h"
#include "zipfile.h"
#include "diagrams/dataexportpanel.h"
#include "diagrams/dataimportpanel.h"
#include "diagrams/diagramdialog.h"
#include "diagrams/graph.h"
#include "diagrams/rectdiagram.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "schematic.h"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>

#include <cmath>
#include <cstring>

namespace de = qucs_s::dataexport;
namespace ds = qucs_s::dataset;
namespace di = qucs_s::dataimport;

namespace {

// A transient's two variables (one complex) over time, an AC analysis's
// over frequency, one swept over time and R1, two operating point values,
// one whose values do not fill what it is over, one with a missing value.
const char* const kDataset =
    "<Qucs Dataset " PACKAGE_VERSION ">\n"
    "<indep time 3>\n  0\n  1e-3\n  2e-3\n</indep>\n"
    "<dep tran.v(out) time>\n  0\n  0.5\n  1\n</dep>\n"
    "<dep i(V1) time>\n  1+j2\n  3-j4\n  0+j0\n</dep>\n"
    "<indep frequency 2>\n  1000\n  1e6\n</indep>\n"
    "<dep ac.v(out) frequency>\n  1+j1\n  0-j0.5\n</dep>\n"
    "<indep R1 2>\n  1000\n  2000\n</indep>\n"
    "<dep sw time R1>\n  1\n  2\n  3\n  4\n  5\n  6\n</dep>\n"
    "<indep v(op) 1>\n  2.5\n</indep>\n"
    "<indep i(op) 1>\n  -0.001\n</indep>\n"
    "<dep bad time>\n  1\n  2\n</dep>\n"
    "<dep nanvar time>\n  1\n  x\n  3\n</dep>\n";

// The values of an .npy: its header's descr and shape, and the doubles.
bool readNpy(const QByteArray& npy, QString* descr, QString* shape, QVector<double>* values)
{
    if (!npy.startsWith(QByteArray("\x93NUMPY\x01\x00", 8))) return false;
    const int length = quint8(npy.at(8)) | (quint8(npy.at(9)) << 8);
    if ((10 + length) % 64 != 0) return false;
    const QString header = QString::fromLatin1(npy.mid(10, length));
    static const QRegularExpression d(QStringLiteral("'descr': '([^']*)'")), s(QStringLiteral("'shape': \\(([^)]*)\\)"));
    *descr = d.match(header).captured(1);
    *shape = s.match(header).captured(1);
    values->clear();
    for (qsizetype at = 10 + length; at + 8 <= npy.size(); at += 8) {
        double v;
        std::memcpy(&v, npy.constData() + at, 8);
        values->append(v);
    }
    return header.endsWith('\n');
}

} // namespace

class TestDataExport : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    ds::Dataset data;

    QString write(const QString& name, const QByteArray& bytes)
    {
        const QString path = dir.filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        return path;
    }
    QByteArray encoded(const QStringList& chosen, de::Format format, ds::Form form = ds::Form::RealImaginary, QString* error = nullptr,
                       de::Written* written = nullptr)
    {
        QByteArray bytes;
        QString why;
        const bool ok = de::encode(data, chosen, {format, form}, &bytes, written, &why);
        if (error != nullptr) *error = why;
        return ok ? bytes : QByteArray();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QVERIFY(data.read(write("run.dat.ngspice", kDataset)));
    }

    // The variables chosen in tables: one for each sweep; an independent
    // one in a table over it, else its own; the operating point's values
    // a row; what does not fit left out, and said.
    void tablesByTheirSweep()
    {
        QStringList notes;
        QList<de::Table> t = de::tablesOf(data, {"tran.v(out)", "ac.v(out)", "i(V1)"}, &notes);
        QCOMPARE(t.size(), 2);
        QCOMPARE(t.at(0).independents, QStringList{"time"});
        QCOMPARE(t.at(0).dependents, (QStringList{"tran.v(out)", "i(V1)"}));
        QCOMPARE(t.at(0).rows, 3);
        QCOMPARE(t.at(1).name(), QStringLiteral("frequency"));
        QVERIFY(notes.isEmpty());
        t = de::tablesOf(data, {"time"});
        QVERIFY(t.size() == 1 && t.first().independents == QStringList{"time"} && t.first().dependents.isEmpty());
        QCOMPARE(de::tablesOf(data, {"time", "tran.v(out)"}).size(), 1);
        t = de::tablesOf(data, {"sw", "time", "R1"});
        QVERIFY(t.size() == 1 && t.first().independents == (QStringList{"time", "R1"}) && t.first().rows == 6);
        t = de::tablesOf(data, {"v(op)", "i(op)"});
        QVERIFY(t.size() == 1 && t.first().independents.isEmpty() && t.first().rows == 1);
        QCOMPARE(t.first().name(), QStringLiteral("operating point"));
        notes.clear();
        QVERIFY(de::tablesOf(data, {"bad", "nothere"}, &notes).isEmpty());
        QCOMPARE(notes, (QStringList{"bad: 2 values, where what it is over makes 3 - left out.", "nothere: not in the dataset."}));
    }

    // CSV: a row of names, then the numbers as short as they read back;
    // a complex variable's two columns in each form; a sweep's rows with
    // the independent values going round; the operating point a row.
    void csvAndItsForms()
    {
        QCOMPARE(QString::fromUtf8(encoded({"tran.v(out)", "i(V1)"}, de::Format::Csv)),
                 QStringLiteral("time,tran.v(out),real(i(V1)),imag(i(V1))\n0,0,1,2\n0.001,0.5,3,-4\n0.002,1,0,0\n"));
        QCOMPARE(QString::fromUtf8(encoded({"sw"}, de::Format::Csv)),
                 QStringLiteral("time,R1,sw\n0,1000,1\n0.001,1000,2\n0.002,1000,3\n0,2000,4\n0.001,2000,5\n0.002,2000,6\n"));
        QCOMPARE(QString::fromUtf8(encoded({"v(op)", "i(op)"}, de::Format::Csv)), QStringLiteral("v(op),i(op)\n2.5,-0.001\n"));
        QCOMPARE(QString::fromUtf8(encoded({"tran.v(out)"}, de::Format::Tsv)), QStringLiteral("time\ttran.v(out)\n0\t0\n0.001\t0.5\n0.002\t1\n"));
        // Magnitude and phase, dB and phase.
        const auto rows = [this](ds::Form form) {
            QList<QStringList> out;
            for (const QString& line : QString::fromUtf8(encoded({"i(V1)"}, de::Format::Csv, form)).split('\n', Qt::SkipEmptyParts))
                out << line.split(',');
            return out;
        };
        QList<QStringList> r = rows(ds::Form::MagnitudePhase);
        QCOMPARE(r.at(0), (QStringList{"time", "mag(i(V1))", "phase(i(V1))"}));
        QCOMPARE(r.at(1).at(1).toDouble(), std::sqrt(5.0));
        QCOMPARE(r.at(2).at(1).toDouble(), 5.0);
        QVERIFY(std::abs(r.at(1).at(2).toDouble() - 63.43494882292201) < 1e-12);
        QVERIFY(std::abs(r.at(2).at(2).toDouble() + 53.13010235415598) < 1e-12);
        r = rows(ds::Form::DbPhase);
        QCOMPARE(r.at(0).at(1), QStringLiteral("dB(i(V1))"));
        QVERIFY(std::abs(r.at(2).at(1).toDouble() - 20 * std::log10(5.0)) < 1e-12);
        QCOMPARE(r.at(3).at(1), QStringLiteral("-inf"));   // (0: no dB)
        // A missing value is NaN.
        QVERIFY(QString::fromUtf8(encoded({"nanvar"}, de::Format::Csv)).contains("\n0.001,NaN\n"));
        // Read back by the importer: the same numbers.
        const QString csv = write("back.csv", encoded({"tran.v(out)", "i(V1)"}, de::Format::Csv));
        di::Data back;
        QString error;
        QVERIFY2(di::read(csv, {}, &back, &error), qPrintable(error));
        QCOMPARE(back.x, QStringLiteral("time"));
        QStringList names;
        for (const ds::Variable& v : back.variables) names << v.name;
        QCOMPARE(names, (QStringList{"time", "tran.v(out)", "real(i(V1))", "imag(i(V1))"}));
        QCOMPARE(back.variables.at(3).re, (QVector<double>{2, -4, 0}));
        QCOMPARE(back.variables.at(0).re, data.find("time")->re);
    }

    // One table for CSV, TSV and text: variables of two sweeps refused,
    // saying where they go; nothing chosen, nothing written.
    void oneTableFormatsTakeOneSweep()
    {
        for (de::Format f : {de::Format::Csv, de::Format::Tsv, de::Format::Text}) {
            QString error;
            QVERIFY(encoded({"tran.v(out)", "ac.v(out)"}, f, ds::Form::RealImaginary, &error).isEmpty());
            QVERIFY2(error.contains("holds one table, and these are over time; frequency") && error.contains("an Excel workbook"),
                     qPrintable(error));
        }
        QString error;
        QVERIFY(encoded({}, de::Format::Xlsx, ds::Form::RealImaginary, &error).isEmpty());
        QCOMPARE(error, QStringLiteral("Choose the variables to export."));
        QVERIFY(encoded({"nothere"}, de::Format::Npz, ds::Form::RealImaginary, &error).isEmpty());
        QCOMPARE(error, QStringLiteral("nothere: not in the dataset."));
    }

    // An Excel sheet has 1,048,576 rows, the names in one: a sweep of as
    // many points is refused for a workbook, and said where it goes.
    void excelsRows()
    {
        QByteArray big = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 1048576>\n";
        big.reserve(16 << 20);
        for (int i = 0; i < 1048576; ++i) big += QByteArray::number(i) + '\n';
        big += "</indep>\n<dep y time>\n";
        for (int i = 0; i < 1048576; ++i) big += "1\n";
        big += "</dep>\n";
        ds::Dataset huge;
        QVERIFY(huge.read(write("huge.dat", big)));
        QByteArray bytes;
        QString error;
        QVERIFY(!de::encode(huge, {"y"}, {de::Format::Xlsx}, &bytes, nullptr, &error));
        QCOMPARE(error, QStringLiteral("An Excel sheet takes 1048576 rows and 16384 columns; time has 1048577 rows and 2 columns. "
                                       "CSV or NumPy take it."));
        QVERIFY(de::encode(huge, {"y"}, {de::Format::Npz}, &bytes, nullptr, &error));
    }

    // Text in columns: its names in a comment, the columns lined up; the
    // importer reads it back.
    void textInColumns()
    {
        const QString text = QString::fromUtf8(encoded({"tran.v(out)", "nanvar"}, de::Format::Text));
        const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 5);
        QVERIFY2(lines.at(0).startsWith("# run.dat.ngspice, written by Qucs-S"), qPrintable(lines.at(0)));
        QCOMPARE(lines.at(1), QStringLiteral("#   time  tran.v(out)  nanvar"));
        QCOMPARE(lines.at(3), QStringLiteral("   0.001          0.5     NaN"));
        di::Data back;
        QString error;
        QVERIFY2(di::read(write("back.txt", text.toUtf8()), {}, &back, &error), qPrintable(error));
        QCOMPARE(back.variables.size(), 3);
        QCOMPARE(back.variables.at(1).name, QStringLiteral("tran.v(out)"));
        QCOMPARE(back.variables.at(1).re, data.find("tran.v(out)")->re);
        QVERIFY(std::isnan(back.variables.at(2).re.at(1)));
    }

    // An Excel workbook: a sheet for each sweep, numbers as numbers, a
    // missing value an empty cell.
    void aWorkbookHasASheetForEachSweep()
    {
        de::Written w;
        const QByteArray bytes = encoded({"tran.v(out)", "ac.v(out)", "nanvar"}, de::Format::Xlsx, ds::Form::RealImaginary, nullptr, &w);
        QVERIFY(!bytes.isEmpty());
        QVERIFY(w.tables == 2 && w.rows == 3 && w.columns == 6);
        qucs_s::sheet::Workbook book;
        QString error;
        QVERIFY2(qucs_s::sheet::readXlsx(bytes, book, &error), qPrintable(error));
        QCOMPARE(book.sheets.size(), 2);
        QCOMPARE(book.sheets.at(0).name, QStringLiteral("time"));
        QCOMPARE(book.sheets.at(1).name, QStringLiteral("frequency"));
        const qucs_s::sheet::Sheet& time = book.sheets.at(0);
        QCOMPARE(time.at(0, 1).text, QStringLiteral("tran.v(out)"));
        QCOMPARE(time.at(2, 0).kind, qucs_s::sheet::Cell::Kind::Number);
        QCOMPARE(time.at(2, 0).value, QStringLiteral("0.001"));
        QCOMPARE(time.at(2, 2).kind, qucs_s::sheet::Cell::Kind::Empty);   // (NaN)
        QCOMPARE(time.at(3, 2).value, QStringLiteral("3"));
        const qucs_s::sheet::Sheet& ac = book.sheets.at(1);
        QCOMPARE(ac.at(0, 2).text, QStringLiteral("imag(ac.v(out))"));
        QCOMPARE(ac.at(2, 2).value, QStringLiteral("-0.5"));
        // Read back by the importer: its first sheet.
        di::Data back;
        QVERIFY2(di::read(write("back.xlsx", bytes), {}, &back, &error), qPrintable(error));
        QCOMPARE(back.sheets, (QStringList{"time", "frequency"}));
        QCOMPARE(back.variables.at(1).re, data.find("tran.v(out)")->re);
    }

    // NumPy: an array for each variable, by its name - independent ones of
    // one dimension, a swept one shaped by what it is over (the last the
    // fastest), complex ones complex; the header padded to 64 bytes.
    void numpyArrays()
    {
        de::Written w;
        const QByteArray bytes = encoded({"tran.v(out)", "i(V1)", "sw", "v(op)"}, de::Format::Npz, ds::Form::RealImaginary, nullptr, &w);
        QCOMPARE(w.arrays, 6);
        QString error;
        const QList<qucs_s::zip::Entry> entries = qucs_s::zip::read(bytes, &error);
        QStringList names;
        for (const auto& e : entries) names << e.name;
        QCOMPARE(names, (QStringList{"time.npy", "tran.v(out).npy", "i(V1).npy", "R1.npy", "sw.npy", "v(op).npy"}));
        QString descr, shape;
        QVector<double> values;
        QVERIFY(readNpy(entries.at(1).data, &descr, &shape, &values));
        QVERIFY(descr == "<f8" && shape == "3," && values == (QVector<double>{0, 0.5, 1}));
        QVERIFY(readNpy(entries.at(2).data, &descr, &shape, &values));
        QVERIFY(descr == "<c16" && shape == "3," && values == (QVector<double>{1, 2, 3, -4, 0, 0}));
        QVERIFY(readNpy(entries.at(4).data, &descr, &shape, &values));
        QCOMPARE(shape, QStringLiteral("2, 3"));
        QCOMPARE(values, (QVector<double>{1, 2, 3, 4, 5, 6}));
        QVERIFY(readNpy(entries.at(5).data, &descr, &shape, &values));
        QVERIFY(shape == "1," && values == QVector<double>{2.5});
        // The importer reads a one-dimensional one back.
        di::Data back;
        QVERIFY2(di::read(write("back.npz", encoded({"tran.v(out)"}, de::Format::Npz)), {}, &back, &error), qPrintable(error));
        bool found = false;
        for (const ds::Variable& v : back.variables) found = found || (v.name == "tran.v(out)" && v.re == data.find("tran.v(out)")->re);
        QVERIFY(found);
    }

    // A Qucs-S dataset: the variables as they are, with the independent
    // ones they are over, and no origin (it was not imported).
    void aDataset()
    {
        const QString path = dir.filePath("part.dat");
        de::Written w;
        QString error;
        QVERIFY2(de::write(path, data, {"i(V1)", "sw"}, {de::Format::Dataset, ds::Form::MagnitudePhase}, &w, &error), qPrintable(error));
        QCOMPARE(w.arrays, 4);
        ds::Dataset back;
        QVERIFY(back.read(path));
        QStringList names;
        for (const ds::Variable& v : back.variables()) names << v.name;
        QCOMPARE(names, (QStringList{"time", "R1", "i(V1)", "sw"}));
        QCOMPARE(back.find("i(V1)")->re, data.find("i(V1)")->re);
        QCOMPARE(back.find("i(V1)")->im, data.find("i(V1)")->im);
        QCOMPARE(back.find("sw")->dependencies, (QStringList{"time", "R1"}));
        di::Origin origin;
        QVERIFY(!di::originOf(path, &origin));
    }

    // The Export tab: the datasets of the folder, the variables of the one
    // chosen, what would be written said and the button only when it can.
    void theExportTab()
    {
        const QString folder = QFileInfo(dir.filePath("tab")).absoluteFilePath();
        write("tab/amp.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n");
        write("tab/amp.dat.ngspice", kDataset);
        di::Imported bench;
        QString error;
        QVERIFY2(di::importFile(folder, write("bench.csv", "time,vout\n0,1\n1,2\n"), {}, &bench, &error), qPrintable(error));

        DataExportPanel panel(folder);
        // Read when it is shown: not before.
        panel.follow(folder + "/amp.dat.ngspice");
        QCOMPARE(panel.list()->rowCount(), 0);
        panel.show();
        QVERIFY(panel.list()->rowCount() > 0);
        QStringList listed;
        for (int i = 0; i < panel.datasetBox()->count(); ++i) listed << panel.datasetBox()->itemText(i);
        QCOMPARE(listed, (QStringList{"amp.dat.ngspice", "bench.dat  (imported)"}));
        panel.follow(folder + "/amp.dat.ngspice");
        QCOMPARE(panel.dataset(), folder + "/amp.dat.ngspice");
        QHash<QString, QString> kinds;
        for (int r = 0; r < panel.list()->rowCount(); ++r) kinds.insert(panel.list()->item(r, 0)->text(), panel.list()->item(r, 1)->text());
        QCOMPARE(kinds.value("v(op)"), QStringLiteral("value"));
        QCOMPARE(kinds.value("i(V1)"), QStringLiteral("dependent, complex"));
        QCOMPARE(kinds.value("frequency"), QStringLiteral("independent"));
        QCOMPARE(panel.summary()->text(), QStringLiteral("Check the variables to export."));
        QVERIFY(!panel.exportButton()->isEnabled());

        // Two sweeps: not for CSV; a workbook takes them, a sheet each.
        panel.choose({"tran.v(out)", "ac.v(out)"});
        QCOMPARE(panel.chosen(), (QStringList{"tran.v(out)", "ac.v(out)"}));
        QVERIFY2(panel.summary()->text().contains("CSV holds one table"), qPrintable(panel.summary()->text()));
        QVERIFY(!panel.exportButton()->isEnabled());
        panel.formatBox()->setCurrentIndex(panel.formatBox()->findData(int(de::Format::Xlsx)));
        QCOMPARE(panel.summary()->text(),
                 QStringLiteral("A sheet for each: over time, 3 rows of 2 columns; over frequency, 2 rows of 3 columns."));
        QVERIFY(panel.exportButton()->isEnabled());
        QVERIFY(panel.complexBox()->isEnabled());
        panel.formatBox()->setCurrentIndex(panel.formatBox()->findData(int(de::Format::Npz)));
        QVERIFY(!panel.complexBox()->isEnabled());   // (NumPy keeps it complex)
        QCOMPARE(panel.summary()->text(), QStringLiteral("An array for each: time, tran.v(out), frequency, ac.v(out)."));

        // Written; not onto the dataset itself.
        panel.formatBox()->setCurrentIndex(panel.formatBox()->findData(int(de::Format::Csv)));
        panel.choose({"tran.v(out)"});
        QVERIFY(!panel.complexBox()->isEnabled());
        const QString csv = dir.filePath("out/amp.csv");
        QDir().mkpath(dir.filePath("out"));
        QVERIFY2(panel.exportTo(csv, &error), qPrintable(error));
        QCOMPARE(panel.status()->text(), QStringLiteral("Wrote %1: 3 rows of 2 columns.").arg(QDir::toNativeSeparators(csv)));
        QFile f(csv);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("time,tran.v(out)\n0,0\n0.001,0.5\n0.002,1\n"));
        QVERIFY(!panel.exportTo(folder + "/amp.dat.ngspice", &error));
        QVERIFY2(error.contains("is the dataset itself"), qPrintable(error));
        // A dataset written beside it: listed, to plot and export in turn.
        panel.formatBox()->setCurrentIndex(panel.formatBox()->findData(int(de::Format::Dataset)));
        QVERIFY(panel.exportTo(folder + "/amp_export.dat", &error));
        QVERIFY(panel.datasetBox()->findData(folder + "/amp_export.dat") >= 0);

        // The diagram's traces: of the dataset shown, else of the first here.
        panel.setTraces([folder] {
            return QList<QPair<QString, QString>>{{folder + "/bench.dat", "vout"}, {folder + "/elsewhere.dat", "x"}};
        });
        panel.tracesButton()->click();
        QCOMPARE(panel.dataset(), folder + "/bench.dat");
        QCOMPARE(panel.chosen(), QStringList{"vout"});
        QCOMPARE(panel.status()->text(), QStringLiteral("The diagram's traces: vout."));
        // Chosen here: the Data tab's is not followed any more.
        panel.follow(folder + "/amp.dat.ngspice");
        QCOMPARE(panel.dataset(), folder + "/bench.dat");

        // A schematic not saved: nothing to export.
        DataExportPanel none{QString()};
        QVERIFY(!none.datasetBox()->isEnabled() && !none.exportButton()->isEnabled());
    }

    // In the diagram's dialog: after Import; the Data tab's dataset shown,
    // and the diagram's traces checked.
    void inTheDiagramsDialog()
    {
        const QString folder = QFileInfo(dir.filePath("dialog")).absoluteFilePath();
        write("dialog/amp.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n");
        write("dialog/amp.dat.ngspice", kDataset);
        write("dialog/other.dat", "<Qucs Dataset " PACKAGE_VERSION ">\n<indep x 2>\n  1\n  2\n</indep>\n<dep y x>\n  3\n  4\n</dep>\n");
        const int simulator = QucsSettings.DefaultSimulator;
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        {
            Schematic sch(nullptr, folder + "/amp.sch");
            RectDiagram d;
            d.Graphs.append(new Graph(&d, "ngspice/tran.v(out)"));
            d.Graphs.append(new Graph(&d, "ngspice/ac.v(out)"));
            auto* dialog = new DiagramDialog(&d, &sch);
            auto* tabs = dialog->findChild<QTabWidget*>();
            QStringList titles;
            for (int i = 0; i < tabs->count(); ++i) titles << tabs->tabText(i);
            QCOMPARE(titles, (QStringList{"Data", "Properties", "Limits", "Theme", "Import", "Export"}));
            auto* panel = dialog->findChild<DataExportPanel*>();
            QVERIFY(panel != nullptr && panel->folder() == folder);
            QCOMPARE(panel->dataset(), folder + "/amp.dat.ngspice");
            QCOMPARE(panel->list()->rowCount(), 0);   // (not read: the tab is not shown)
            panel->tracesButton()->click();
            QCOMPARE(panel->chosen(), (QStringList{"tran.v(out)", "ac.v(out)"}));
            // The Data tab's dataset followed.
            auto* datasets = dialog->findChild<QComboBox*>("diagramDataset");
            datasets->setCurrentIndex(datasets->findData("other"));
            QMetaObject::invokeMethod(dialog, "slotReadVarsAndSetSimulator", Q_ARG(int, 0));
            QCOMPARE(panel->dataset(), folder + "/other.dat");
            // A file imported in the Import tab: listed here too.
            QString error;
            dialog->findChild<DataImportPanel*>()->importFiles({write("measured.csv", "t,v\n0,1\n1,2\n")});
            QVERIFY(panel->datasetBox()->findText("measured.dat  (imported)") >= 0);
            dialog->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        QucsSettings.DefaultSimulator = simulator;
    }
    // QUCS_TEST_EXPORT_DIR=<dir>: a file of each format there, of the
    // transient and the AC analysis (Excel, NumPy: both), to open in the
    // programs they are for; with QUCS_TEST_GRAB=<dir>, a picture of the tab.
    void samplesForOtherPrograms()
    {
        const QString out = qEnvironmentVariable("QUCS_TEST_EXPORT_DIR"), grabs = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (out.isEmpty() && grabs.isEmpty()) QSKIP("set QUCS_TEST_EXPORT_DIR (files) or QUCS_TEST_GRAB (a picture) to a folder");
        if (!out.isEmpty())
            for (de::Format f : de::formats()) {
                const QStringList chosen = de::oneTable(f) ? QStringList{"tran.v(out)", "i(V1)"}
                                                           : QStringList{"tran.v(out)", "i(V1)", "ac.v(out)", "sw", "v(op)", "i(op)"};
                QString error;
                QVERIFY2(de::write(out + "/sample." + de::suffixOf(f), data, chosen, {f, ds::Form::RealImaginary}, nullptr, &error),
                         qPrintable(error));
            }
        if (!grabs.isEmpty()) {
            const QString folder = QFileInfo(dir.filePath("grab")).absoluteFilePath();
            write("grab/rc.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n");
            write("grab/rc.dat.ngspice", kDataset);
            const int simulator = QucsSettings.DefaultSimulator;
            QucsSettings.DefaultSimulator = spicecompat::simNgspice;
            {
                Schematic sch(nullptr, folder + "/rc.sch");
                RectDiagram d;
                d.Graphs.append(new Graph(&d, "ngspice/tran.v(out)"));
                d.Graphs.append(new Graph(&d, "ngspice/ac.v(out)"));
                auto* dialog = new DiagramDialog(&d, &sch);
                dialog->resize(820, 560);
                dialog->show();
                auto* panel = dialog->findChild<DataExportPanel*>();
                dialog->findChild<QTabWidget*>()->setCurrentWidget(panel);
                panel->tracesButton()->click();
                panel->formatBox()->setCurrentIndex(panel->formatBox()->findData(int(de::Format::Xlsx)));
                QTest::qWait(50);
                dialog->grab().save(grabs + "/export-tab.png");
                dialog->close();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            QucsSettings.DefaultSimulator = simulator;
        }
    }
};

QTEST_MAIN(TestDataExport)
#include "test_data_export.moc"
