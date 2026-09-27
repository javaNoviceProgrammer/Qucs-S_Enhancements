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
#include "textcodec.h"
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
        QVERIFY(book.encoding.bom);
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

        // Not UTF-8 (a Windows program's ANSI), read and written as it was;
        // no final line break kept.
        const QByteArray latin = "Wert;Einheit\n5;\xB5" "F";
        const sheet::Workbook l = sheet::readCsv(latin);
        QCOMPARE(l.encoding.kind, qucs_s::textcodec::Encoding::Kind::Windows1252);
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

    // A CSV file in a Windows program's ANSI code page: "Ω" typed into it
    // made UTF-8 with the mark Excel reads UTF-8 by - it was written as
    // "?"; a UTF-16 file (Excel's "Unicode Text") read as text, not NULs,
    // and written back as it was (bug hunt 2026-09-26, A7).
    void csvEncodingsAreKept()
    {
        sheet::Workbook ansi = sheet::readCsv("part;value\nR1;10 k\xB5\nC1;5 \x80\n");
        QCOMPARE(ansi.encoding.kind, textcodec::Encoding::Kind::Windows1252);
        QCOMPARE(ansi.sheets.first().at(1, 1).text, QString::fromUtf8("10 kµ"));
        QCOMPARE(ansi.sheets.first().at(2, 1).text, QString::fromUtf8("5 €"));
        QVERIFY(sheet::unencodable(ansi.sheets.first(), ansi).isEmpty());
        QCOMPARE(sheet::writeCsv(ansi.sheets.first(), ansi), QByteArray("part;value\nR1;10 k\xB5\nC1;5 \x80\n"));
        sheet::enter(ansi.sheets.first().cell(1, 1), QString::fromUtf8("4.7 kΩ"), ansi);
        QCOMPARE(sheet::unencodable(ansi.sheets.first(), ansi), QString::fromUtf8("Ω"));
        const QByteArray saved = sheet::writeCsv(ansi.sheets.first(), ansi);
        QCOMPARE(saved, QByteArray("\xEF\xBB\xBF") + QString::fromUtf8("part;value\nR1;4.7 kΩ\nC1;5 €\n").toUtf8());
        const sheet::Workbook again = sheet::readCsv(saved);
        QCOMPARE(again.sheets.first().at(1, 1).text, QString::fromUtf8("4.7 kΩ"));

        QStringEncoder le(QStringConverter::Utf16LE);
        const QByteArray unicode = QByteArray("\xFF\xFE") + QByteArray(le.encode(QString::fromUtf8("part\tvalue\r\nR1\t4.7 kΩ\r\n")));
        const sheet::Workbook u = sheet::readCsv(unicode);
        QCOMPARE(u.encoding.kind, textcodec::Encoding::Kind::Utf16LE);
        QCOMPARE(u.delimiter, QChar('\t'));
        QCOMPARE(u.newline, QString("\r\n"));
        QCOMPARE(u.sheets.first().at(0, 0).text, QString("part"));
        QCOMPARE(u.sheets.first().at(1, 1).text, QString::fromUtf8("4.7 kΩ"));
        QCOMPARE(sheet::writeCsv(u.sheets.first(), u), unicode);
    }

    // A formula filled over cells, as Excel moves it: relative references
    // moved, absolute ones and what is quoted kept.
    void formulasAreMovedAsExcelFillsThem()
    {
        QCOMPARE(sheet::shiftedFormula("A1*2", 1, 0), QString("A2*2"));
        QCOMPARE(sheet::shiftedFormula("$A$1+A1+SUM(A$1:A1)", 2, 0), QString("$A$1+A3+SUM(A$1:A3)"));
        QCOMPARE(sheet::shiftedFormula("$A1+B$1", 1, 1), QString("$A2+C$1"));
        QCOMPARE(sheet::shiftedFormula("LOG10(A1)+ATAN2(B1,C1)", 1, 1), QString("LOG10(B2)+ATAN2(C2,D2)"));
        QCOMPARE(sheet::shiftedFormula("\"A1\"&A1", 1, 0), QString("\"A1\"&A2"));
        QCOMPARE(sheet::shiftedFormula("'Sheet 1'!A1+Sheet2!B$2", 1, 1), QString("'Sheet 1'!B2+Sheet2!C$2"));
        QCOMPARE(sheet::shiftedFormula("SUM(A:A)+SUM(1:1)+SUM($A:B)", 1, 1), QString("SUM(B:B)+SUM(2:2)+SUM($A:C)"));
        QCOMPARE(sheet::shiftedFormula("Table1[Col1]+A1", 1, 0), QString("Table1[Col1]+A2"));
        QCOMPARE(sheet::shiftedFormula("1.5E3*A1+X1Y", 1, 0), QString("1.5E3*A2+X1Y"));
        QCOMPARE(sheet::shiftedFormula("Z9+AA10", 0, 1), QString("AA9+AB10"));
        QCOMPARE(sheet::shiftedFormula("A1", -1, 0), QString("#REF!"));
        QCOMPARE(sheet::shiftedFormula("XFD1", 0, 1), QString("#REF!"));
        QCOMPARE(sheet::shiftedFormula("A1*2", 0, 0), QString("A1*2"));
    }

    // Any text as a formula, moved any way: no crash, nothing read past
    // its end; not moved, it is as it was. Seeded.
    void anyFormulaIsMovedSafely()
    {
        QRandomGenerator random(926);
        const QString alphabet = QStringLiteral("ABCXYZabc0123456789$:!'\"[]()+-*/,. #_\u00e9");
        for (int n = 0; n < 5000; ++n) {
            QString formula;
            const int length = int(random.bounded(24));
            for (int i = 0; i < length; ++i) formula += alphabet.at(random.bounded(int(alphabet.size())));
            QCOMPARE(sheet::shiftedFormula(formula, 0, 0), formula);
            const QString moved = sheet::shiftedFormula(formula, int(random.bounded(2000001)) - 1000000, int(random.bounded(40001)) - 20000);
            Q_UNUSED(moved);
        }
        QCOMPARE(sheet::shiftedFormula("A1", 1048575, 0), QString("A1048576"));   // the last row
        QCOMPARE(sheet::shiftedFormula("A1", 1048576, 0), QString("#REF!"));
        QCOMPARE(sheet::shiftedFormula("A1", 1048574, 16383), QString("XFD1048575"));
    }

    // Excel writes a formula filled down as one shared formula: the first
    // cell holds it, the others name it. Their own formulas are read (they
    // showed none), and when the first cell changes they are written each
    // on its own - they named a shared formula that was no more, and Excel
    // called the workbook damaged. An array changed inside is written so
    // too (bug hunt 2026-09-26, A8).
    void sharedFormulasSurviveTheirFirstCell()
    {
        const QByteArray sheetXml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><dimension ref=\"A1:D3\"/><sheetData>"
            "<row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><f t=\"shared\" ref=\"B1:B3\" si=\"0\">A1*2</f><v>2</v></c>"
            "<c r=\"C1\"><f t=\"shared\" ref=\"C1:C3\" si=\"1\">$A$1+A1+SUM(A$1:A1)</f><v>3</v></c>"
            "<c r=\"D1\"><f t=\"array\" ref=\"D1:D3\">A1:A3*10</f><v>10</v></c></row>"
            "<row r=\"2\"><c r=\"A2\"><v>2</v></c><c r=\"B2\"><f t=\"shared\" si=\"0\"/><v>4</v></c>"
            "<c r=\"C2\"><f t=\"shared\" si=\"1\"/><v>6</v></c><c r=\"D2\"><v>20</v></c></row>"
            "<row r=\"3\"><c r=\"A3\"><v>3</v></c><c r=\"B3\"><f t=\"shared\" si=\"0\"/><v>6</v></c>"
            "<c r=\"C3\"><f t=\"shared\" si=\"1\"/><v>10</v></c><c r=\"D3\"><v>30</v></c></row>"
            "</sheetData></worksheet>";
        QList<zip::Entry> package = zip::read(excelLike());
        for (zip::Entry& e : package)
            if (e.name == "xl/worksheets/sheet1.xml") e.data = sheetXml;
        const QByteArray workbook = zip::write(package);

        sheet::Workbook book;
        QString why;
        QVERIFY2(sheet::readXlsx(workbook, book, &why), qPrintable(why));
        const sheet::Sheet& read = book.sheets[0];
        QCOMPARE(read.at(1, 1).formula, QString("A2*2"));
        QCOMPARE(read.at(2, 1).formula, QString("A3*2"));
        QCOMPARE(read.at(2, 1).text, QString("6"));
        QCOMPARE(sheet::editText(read.at(2, 1)), QString("=A3*2"));
        QCOMPARE(read.at(2, 2).formula, QString("$A$1+A3+SUM(A$1:A3)"));

        const auto sheet1 = [](const QByteArray& written) {
            return entry(zip::read(written), "xl/worksheets/sheet1.xml")->data;
        };
        // Its first cell changed.
        sheet::Workbook edited = book;
        sheet::enter(edited.sheets[0].cell(0, 1), "=A1*3", edited);
        edited.sheets[0].changed = true;
        QByteArray xml = sheet1(sheet::writeXlsx(edited));
        QVERIFY2(!xml.contains("si=\"0\""), xml.constData());
        QVERIFY2(xml.contains("<c r=\"B1\"><f>A1*3</f></c>"), xml.constData());
        QVERIFY2(xml.contains("<c r=\"B2\"><f>A2*2</f><v>4</v></c>"), xml.constData());
        QVERIFY2(xml.contains("<c r=\"B3\"><f>A3*2</f><v>6</v></c>"), xml.constData());
        QVERIFY2(xml.contains("<c r=\"C2\"><f t=\"shared\" si=\"1\"/><v>6</v></c>"), xml.constData());   // the other as it was
        QVERIFY2(xml.contains("<f t=\"array\" ref=\"D1:D3\">"), xml.constData());
        sheet::Workbook back;
        QVERIFY(sheet::readXlsx(sheet::writeXlsx(edited), back));
        QCOMPARE(back.sheets[0].at(1, 1).formula, QString("A2*2"));
        QCOMPARE(back.sheets[0].at(2, 2).formula, QString("$A$1+A3+SUM(A$1:A3)"));

        // Cleared.
        sheet::Workbook cleared = book;
        sheet::enter(cleared.sheets[0].cell(0, 1), "", cleared);
        cleared.sheets[0].changed = true;
        xml = sheet1(sheet::writeXlsx(cleared));
        QVERIFY2(!xml.contains("si=\"0\"") && xml.contains("<c r=\"B2\"><f>A2*2</f><v>4</v></c>"), xml.constData());

        // A cell of the other shared formula changed (not its first).
        sheet::Workbook dependent = book;
        sheet::enter(dependent.sheets[0].cell(2, 2), "7", dependent);
        dependent.sheets[0].changed = true;
        xml = sheet1(sheet::writeXlsx(dependent));
        QVERIFY2(!xml.contains("si=\"1\""), xml.constData());
        QVERIFY2(xml.contains("<c r=\"C1\"><f>$A$1+A1+SUM(A$1:A1)</f><v>3</v></c>"), xml.constData());
        QVERIFY2(xml.contains("<c r=\"C2\"><f>$A$1+A2+SUM(A$1:A2)</f><v>6</v></c>"), xml.constData());
        QVERIFY2(xml.contains("<c r=\"C3\"><v>7</v></c>"), xml.constData());

        // A cell inside the array changed: the array written as its first
        // cell's formula, the rest as values.
        sheet::Workbook array = book;
        sheet::enter(array.sheets[0].cell(1, 3), "25", array);
        array.sheets[0].changed = true;
        xml = sheet1(sheet::writeXlsx(array));
        QVERIFY2(!xml.contains("t=\"array\""), xml.constData());
        QVERIFY2(xml.contains("<c r=\"D1\"><f>A1:A3*10</f><v>10</v></c>"), xml.constData());
        QVERIFY2(xml.contains("<c r=\"D2\"><v>25</v></c>") && xml.contains("<c r=\"D3\"><v>30</v></c>"), xml.constData());
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
