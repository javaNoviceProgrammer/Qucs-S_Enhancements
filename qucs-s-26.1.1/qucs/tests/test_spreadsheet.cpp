/*
 * Workbooks read and written: ZIP archives (inflated by our own reader,
 * against archives and streams zlib made); CSV files (quotes, delimiters,
 * encodings, written back as they were); .xlsx workbooks (shared strings,
 * numbers, dates by their styles, booleans, errors, formulas, widths,
 * merged cells) and, a cell changed, written back with every part but the
 * changed sheet's cells as it was - the calculation chain dropped and a
 * calculation asked for; a CSV file saved as a workbook.
 */
#include <QtTest>
#include <QRandomGenerator>

#include "excel_fixture.h"
#include "spreadsheet.h"
#include "zipfile.h"

using namespace qucs_s;
using sheet::Cell;

namespace {




const zip::Entry* entry(const QList<zip::Entry>& package, const QString& name)
{
    for (const zip::Entry& e : package)
        if (e.name == name) return &e;
    return nullptr;
}

} // namespace

class TestSpreadsheet : public QObject
{
    Q_OBJECT

private slots:
    void zipRoundTripsAndReadsZlibs()
    {
        QCOMPARE(zip::crc32("123456789"), 0xCBF43926u);

        // zlib's raw stream (dynamic Huffman codes) inflated.
        bool ok = false;
        const QByteArray text = zip::inflate(QByteArray::fromHex(kZlibRaw), &ok);
        QVERIFY(ok);
        QByteArray expected;
        for (int i = 0; i < kZlibRawRepeat; ++i)
            expected += "The DEFLATE stream of this text was made by zlib (Python), at level 9, raw: a test of the "
                        "inflater against another's deflater. ";
        QCOMPARE(text, expected);
        zip::inflate("\xff\xff\xff", &ok);
        QVERIFY(!ok);

        // Written and read: text (fixed or dynamic codes), nothing, noise
        // (stored), a long run.
        QByteArray noise(5000, 0);
        for (char& c : noise) c = char(QRandomGenerator::global()->bounded(256));
        const QList<zip::Entry> files = {
            {"a.txt", "hello"},
            {"empty", ""},
            {"noise.bin", noise},
            {"dir/long.xml", QByteArray(200000, 'x') + "<end/>"},
            {"ünïcode.txt", "ü"},
        };
        QString why;
        const QList<zip::Entry> back = zip::read(zip::write(files), &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(back.size(), files.size());
        for (int i = 0; i < files.size(); ++i) {
            QCOMPARE(back[i].name, files[i].name);
            QCOMPARE(back[i].data, files[i].data);
        }
        QVERIFY(zip::read("not a zip", &why).isEmpty());
        QVERIFY(!why.isEmpty());

        // Python's zipfile: every part read, checksums right.
        const QList<zip::Entry> excel = zip::read(excelLike(), &why);
        QVERIFY2(!excel.isEmpty(), qPrintable(why));
        QVERIFY(entry(excel, "xl/worksheets/sheet1.xml") != nullptr);
        QVERIFY(entry(excel, "xl/worksheets/sheet1.xml")->data.contains("<sheetData>"));
    }

    // A damaged archive or workbook is refused, never read past its end
    // (the sanitizers watch).
    void damagedFilesAreRefused()
    {
        const QByteArray good = excelLike();
        QRandomGenerator rng(20260926);
        for (int round = 0; round < 300; ++round) {
            QByteArray bad = good;
            const int flips = 1 + int(rng.bounded(8));
            for (int k = 0; k < flips; ++k) bad[int(rng.bounded(bad.size()))] = char(rng.bounded(256));
            if (round % 7 == 0) bad.truncate(int(rng.bounded(bad.size())));
            sheet::Workbook book;
            QString why;
            if (!sheet::readXlsx(bad, book, &why)) QVERIFY(!why.isEmpty());
            bool ok = true;
            zip::inflate(bad.mid(int(rng.bounded(bad.size()))), &ok);
        }
    }

    void columnsAndDates()
    {
        QCOMPARE(sheet::columnName(0), QString("A"));
        QCOMPARE(sheet::columnName(25), QString("Z"));
        QCOMPARE(sheet::columnName(26), QString("AA"));
        QCOMPARE(sheet::columnName(701), QString("ZZ"));
        QCOMPARE(sheet::columnName(702), QString("AAA"));
        int row = -1;
        QCOMPARE(sheet::columnOf("B12", &row), 1);
        QCOMPARE(row, 11);
        QCOMPARE(sheet::columnOf("$AA$3", &row), 26);
        QCOMPARE(row, 2);
        QCOMPARE(sheet::columnOf("12"), -1);

        QCOMPARE(sheet::dateText(45413), QString("2024-05-01"));
        QCOMPARE(sheet::dateText(45413.5625), QString("2024-05-01 13:30"));
        QCOMPARE(sheet::dateText(0.25), QString("06:00"));
        QCOMPARE(sheet::serialOf(QDateTime(QDate(2024, 5, 1), QTime(13, 30))), 45413.5625);
        QCOMPARE(sheet::dateText(0, true), QString("1904-01-01"));
    }

    // Quotes, delimiters and lines in quotes; a detected ;, a tab; a byte
    // order mark and CR LF kept; what was read written back as it was.
    void csvIsReadAndWrittenBack()
    {
        const QByteArray text = "\xEF\xBB\xBF" "name,value,note\r\n"
                                "R1,1k,\"a, b\"\r\n"
                                "C1,1e-9,\"say \"\"hi\"\"\"\r\n"
                                "L1,,\"two\r\nlines\"\r\n"
                                "\"007\",42,\r\n";
        sheet::Workbook book = sheet::readCsv(text);
        QCOMPARE(book.delimiter, QChar(','));
        QVERIFY(book.bom);
        QCOMPARE(book.newline, QString("\r\n"));
        const sheet::Sheet& s = book.sheets.first();
        QCOMPARE(s.rowCount(), 5);
        QCOMPARE(s.at(1, 2).text, QString("a, b"));
        QCOMPARE(s.at(2, 2).text, QString("say \"hi\""));
        QCOMPARE(s.at(3, 2).text, QString("two\r\nlines"));
        QCOMPARE(s.at(3, 1).kind, Cell::Kind::Empty);
        QCOMPARE(s.at(2, 1).kind, Cell::Kind::Number);
        QCOMPARE(s.at(1, 1).kind, Cell::Kind::Text);     // 1k
        QCOMPARE(s.at(4, 0).kind, Cell::Kind::Text);     // "007", quoted
        QCOMPARE(s.at(4, 1).kind, Cell::Kind::Number);
        QCOMPARE(s.at(4, 2).kind, Cell::Kind::Empty);    // the trailing field kept
        QCOMPARE(sheet::writeCsv(s, book), text.left(text.indexOf("\"007\"")) + "007,42,\r\n");

        QCOMPARE(sheet::detectDelimiter("a;b;c\n1;2;3\n4,5;6;7\n"), QChar(';'));
        QCOMPARE(sheet::detectDelimiter("a\tb\n1\t2\n"), QChar('\t'));
        QCOMPARE(sheet::detectDelimiter("f,\"x;y;z\"\n1,2\n"), QChar(','));
        const sheet::Workbook semi = sheet::readCsv("a;b\n1,5;2\n");
        QCOMPARE(semi.sheets.first().at(1, 0).text, QString("1,5"));

        // Latin-1, read and written as it was; no final line break kept.
        const QByteArray latin = "Wert;Einheit\n5;\xB5" "F";
        const sheet::Workbook l = sheet::readCsv(latin);
        QVERIFY(l.latin1);
        QCOMPARE(l.sheets.first().at(1, 1).text, QString::fromUtf8("µF"));
        QCOMPARE(sheet::writeCsv(l.sheets.first(), l), latin);

        // Typed into it: a number or text, as typed (a formula is text).
        Cell c;
        sheet::enter(c, "=A1", book);
        QCOMPARE(c.kind, Cell::Kind::Text);
        QVERIFY(c.formula.isEmpty());
        sheet::enter(c, "1.50", book);
        QCOMPARE(c.kind, Cell::Kind::Number);
        QCOMPARE(c.text, QString("1.50"));
    }

    void xlsxIsRead()
    {
        sheet::Workbook book;
        QString why;
        QVERIFY2(sheet::readXlsx(excelLike(), book, &why), qPrintable(why));
        QCOMPARE(book.format, sheet::Format::Xlsx);
        QCOMPARE(book.sheets.size(), 2);
        const sheet::Sheet& m = book.sheets[0];
        QCOMPARE(m.name, QString("Measurements"));
        QCOMPARE(m.part, QString("xl/worksheets/sheet1.xml"));
        QCOMPARE(book.sheets[1].name, QString("Parts & Values"));

        QCOMPARE(m.at(0, 0).text, QString("Frequency"));
        QCOMPARE(m.at(0, 2).text, QString("Rich text"));          // its runs joined
        QCOMPARE(m.at(0, 3).text, QString("Inline"));
        QCOMPARE(m.at(1, 1).kind, Cell::Kind::Number);
        QCOMPARE(m.at(1, 1).text, QString("0.3"));
        QCOMPARE(m.at(1, 1).value, QString("0.30000000000000004"));
        QCOMPARE(m.at(1, 2).formula, QString("A2*B2"));
        QCOMPARE(m.at(1, 2).text, QString("300"));
        QCOMPARE(m.at(1, 3).kind, Cell::Kind::Boolean);
        QCOMPARE(m.at(1, 3).text, QString("TRUE"));
        QCOMPARE(m.at(2, 0).kind, Cell::Kind::Date);                // numFmt 14
        QCOMPARE(m.at(2, 0).text, QString("2024-05-01"));
        QCOMPARE(m.at(2, 1).text, QString("2024-05-01 13:30"));     // a format of its own
        QCOMPARE(m.at(2, 2).kind, Cell::Kind::Error);
        QCOMPARE(m.at(2, 2).text, QString("#DIV/0!"));
        QCOMPARE(m.at(2, 3).text, QString("xy"));
        QCOMPARE(m.at(2, 3).formula, QString("\"x\"&\"y\""));
        QCOMPARE(m.at(3, 0).kind, Cell::Kind::Empty);               // the row missing
        QCOMPARE(m.at(4, 0).kind, Cell::Kind::Number);              // a percentage: not a date
        QCOMPARE(m.at(4, 1).text, QString::fromUtf8("漢字"));        // not its reading
        QCOMPARE(m.at(4, 2).style, 1);
        QCOMPARE(m.widths.value(0), 14.5);
        QCOMPARE(m.widths.value(2), 9.25);
        QCOMPARE(m.merged, QList<QRect>({QRect(QPoint(1, 4), QPoint(2, 4))}));
        QCOMPARE(book.sheets[1].at(1, 0).text, QString("C1"));        // x: prefixed
        QCOMPARE(book.sheets[1].at(1, 1).text, QString("1e-09"));

        QVERIFY(!sheet::readXlsx("PK junk", book, &why));
    }

    // A cell changed: the sheet's other cells, the other parts - styles,
    // strings, the other sheet - as they were; the calculation chain gone.
    void xlsxIsWrittenBackKeepingWhatItHad()
    {
        sheet::Workbook book;
        QVERIFY(sheet::readXlsx(excelLike(), book));
        const QList<zip::Entry> original = book.package;
        sheet::Sheet& m = book.sheets[0];
        sheet::enter(m.cell(1, 1), "0.5", book);
        sheet::enter(m.cell(2, 0), "2024-06-02", book);        // a date cell
        sheet::enter(m.cell(6, 5), "=SUM(A2:B2)", book);       // beyond the cells there were
        sheet::enter(m.cell(0, 1), "'100", book);              // text, though it looks a number
        sheet::enter(m.cell(4, 2), "", book);                  // cleared, its style kept
        m.changed = true;

        const QByteArray written = sheet::writeXlsx(book);
        QString why;
        const QList<zip::Entry> package = zip::read(written, &why);
        QVERIFY2(!package.isEmpty(), qPrintable(why));
        for (const QString& same : {QStringLiteral("xl/styles.xml"), QStringLiteral("xl/sharedStrings.xml"),
                                    QStringLiteral("xl/worksheets/sheet2.xml"), QStringLiteral("docProps/app.xml"),
                                    QStringLiteral("_rels/.rels")}) {
            QVERIFY2(entry(package, same) != nullptr, qPrintable(same));
            QCOMPARE(entry(package, same)->data, entry(original, same)->data);
        }
        QVERIFY(entry(package, "xl/calcChain.xml") == nullptr);
        QVERIFY(!entry(package, "xl/_rels/workbook.xml.rels")->data.contains("calcChain"));
        QVERIFY(!entry(package, "[Content_Types].xml")->data.contains("calcChain"));
        const QByteArray workbook = entry(package, "xl/workbook.xml")->data;
        QVERIFY(workbook.contains("</definedNames><calcPr fullCalcOnLoad=\"1\"/>"));   // in its place
        const QByteArray sheet1 = entry(package, "xl/worksheets/sheet1.xml")->data;
        QVERIFY(sheet1.contains("<dimension ref=\"A1:F7\"/>"));
        QVERIFY(sheet1.contains("<row r=\"1\" ht=\"20.25\" customHeight=\"1\" x14ac:dyDescent=\"0.25\">"));
        QVERIFY(sheet1.contains("<c r=\"C2\"><f>A2*B2</f><v>300.00000000000006</v></c>"));   // as it was
        QVERIFY(sheet1.contains("<mergeCell ref=\"B5:C5\"/>"));
        QVERIFY(sheet1.contains("<cols>"));
        QVERIFY(sheet1.contains("<c r=\"C5\" s=\"1\"/>"));

        sheet::Workbook again;
        QVERIFY2(sheet::readXlsx(written, again, &why), qPrintable(why));
        const sheet::Sheet& n = again.sheets[0];
        QCOMPARE(n.at(1, 1).text, QString("0.5"));
        QCOMPARE(n.at(1, 0).text, QString("1000"));
        QCOMPARE(n.at(2, 0).text, QString("2024-06-02"));
        QCOMPARE(n.at(2, 0).kind, Cell::Kind::Date);
        QCOMPARE(n.at(6, 5).formula, QString("SUM(A2:B2)"));
        QCOMPARE(n.at(0, 1).kind, Cell::Kind::Text);
        QCOMPARE(n.at(0, 1).text, QString("100"));
        QCOMPARE(n.at(4, 1).text, QString::fromUtf8("漢字"));
        QCOMPARE(again.sheets[1].at(1, 1).text, QString("1e-09"));

        // The prefixed sheet changed: its cells written with its prefix.
        sheet::enter(again.sheets[1].cell(2, 0), "added", again);
        again.sheets[1].changed = true;
        sheet::Workbook third;
        QVERIFY(sheet::readXlsx(sheet::writeXlsx(again), third));
        QCOMPARE(third.sheets[1].at(2, 0).text, QString("added"));
        QCOMPARE(third.sheets[1].at(0, 0).text, QString("R1"));
        QVERIFY(entry(third.package, "xl/worksheets/sheet2.xml")->data.contains("<x:row r=\"3\"><x:c r=\"A3\" t=\"inlineStr\">"));

        // Nothing changed: the parts all as they were.
        sheet::Workbook unchanged;
        QVERIFY(sheet::readXlsx(excelLike(), unchanged));
        const QList<zip::Entry> same = zip::read(sheet::writeXlsx(unchanged));
        QCOMPARE(same.size(), original.size());
        for (int i = 0; i < same.size(); ++i) QCOMPARE(same[i].data, original[i].data);
    }

    // What is typed into a workbook's cell.
    void typedIntoAWorkbook()
    {
        sheet::Workbook book;
        QVERIFY(sheet::readXlsx(excelLike(), book));
        Cell c;
        sheet::enter(c, "=A1+1", book);
        QCOMPARE(c.formula, QString("A1+1"));
        QCOMPARE(sheet::editText(c), QString("=A1+1"));
        sheet::enter(c, "true", book);
        QCOMPARE(c.kind, Cell::Kind::Boolean);
        QCOMPARE(c.text, QString("TRUE"));
        sheet::enter(c, "1e3", book);
        QCOMPARE(c.kind, Cell::Kind::Number);
        QCOMPARE(c.text, QString("1000"));
        sheet::enter(c, "2024-06-02", book);   // not a date cell: text
        QCOMPARE(c.kind, Cell::Kind::Text);
        c.style = 2;
        sheet::enter(c, "2024-06-02 08:15", book);
        QCOMPARE(c.kind, Cell::Kind::Date);
        QCOMPARE(c.text, QString("2024-06-02 08:15"));
        QCOMPARE(c.style, 2);
    }

    // A CSV file saved as a workbook: its numbers numbers, its text text.
    void csvSavedAsAWorkbook()
    {
        sheet::Workbook book = sheet::readCsv("part,value\nR1,1000\nC1,1e-9\n");
        book.sheets.first().name = "bom: [draft]";
        sheet::Workbook x;
        QString why;
        QVERIFY2(sheet::readXlsx(sheet::writeXlsx(book), x, &why), qPrintable(why));
        QCOMPARE(x.sheets.size(), 1);
        QCOMPARE(x.sheets.first().name, QString("bom draft"));   // what a name may not have taken out
        QCOMPARE(x.sheets.first().at(1, 1).kind, Cell::Kind::Number);
        QCOMPARE(x.sheets.first().at(1, 1).text, QString("1000"));
        QCOMPARE(x.sheets.first().at(2, 0).text, QString("C1"));
        QCOMPARE(x.sheets.first().at(2, 1).text, QString("1e-09"));

        // And the workbook's sheet saved as CSV.
        QTemporaryDir dir;
        const QString csv = dir.filePath("out.csv");
        QVERIFY(sheet::writeFile(csv, x, 0, &why));
        QFile f(csv);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("part,value\nR1,1000\nC1,1e-09\n"));
        QVERIFY(!sheet::readFile(dir.filePath("old.xls"), x, &why));
    }
};

QTEST_MAIN(TestSpreadsheet)
#include "test_spreadsheet.moc"
