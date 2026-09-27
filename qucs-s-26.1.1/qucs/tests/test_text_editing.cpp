/*
 * The text editor keeps what is in a file (bug hunt 2026-09-26, A4-A6):
 * Edit > Comment/Uncomment changes the lines selected and nothing else, in
 * one step; Undo right after opening does not empty the document; a file
 * is read as its bytes are - UTF-8, UTF-16 with its mark, Windows-1252 -
 * and written back so, its line ends too, and a character its encoding
 * has no bytes for is asked about, never written as "?" or U+FFFD.
 */
#include <QtTest>
#include <QAbstractTextDocumentLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "textcodec.h"
#include "textdoc.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using qucs_s::textcodec::Encoding;

namespace {

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool write(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

// Selects from \a start to \a end (character positions) in \a doc.
void select(TextDoc& doc, int start, int end)
{
    QTextCursor c(doc.document());
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    doc.setTextCursor(c);
}

// Runs fn and answers the "Save as UTF-8?" question with \a yes; whether
// it was asked.
bool answeringUtf8(const std::function<void()>& fn, bool yes, QString* asked = nullptr)
{
    bool was = false;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr || box->objectName() != "saveAsUtf8" || !box->isVisible()) return;
        was = true;
        if (asked != nullptr) *asked = box->text();
        for (QAbstractButton* b : box->buttons())
            if ((yes && b->text() == "Save as UTF-8") || (!yes && box->buttonRole(b) == QMessageBox::RejectRole)) {
                b->click();
                return;
            }
    });
    timer.start(20);
    fn();
    return was;
}

} // namespace

class TestTextEditing : public QObject
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
        QucsSettings.WriteTextDocSettings = false;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // Bytes read and written back as they were, whatever the encoding.
    void encodingsAreFoundAndKept_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("bom");
        QTest::addColumn<QString>("text");
        const QString ohm = QString::fromUtf8("R = 4.7 kΩ, 25 °C\n");
        QTest::newRow("utf-8") << ohm.toUtf8() << int(Encoding::Kind::Utf8) << false << ohm;
        QTest::newRow("utf-8 marked") << QByteArray("\xEF\xBB\xBF") + ohm.toUtf8() << int(Encoding::Kind::Utf8) << true << ohm;
        QStringEncoder le(QStringConverter::Utf16LE), be(QStringConverter::Utf16BE), le32(QStringConverter::Utf32LE),
            be32(QStringConverter::Utf32BE);
        QTest::newRow("utf-16 le") << QByteArray("\xFF\xFE") + QByteArray(le.encode(ohm)) << int(Encoding::Kind::Utf16LE) << true << ohm;
        QTest::newRow("utf-16 be") << QByteArray("\xFE\xFF") + QByteArray(be.encode(ohm)) << int(Encoding::Kind::Utf16BE) << true << ohm;
        QTest::newRow("utf-32 le") << QByteArray("\xFF\xFE\x00\x00", 4) + QByteArray(le32.encode(ohm)) << int(Encoding::Kind::Utf32LE) << true
                                   << ohm;
        QTest::newRow("utf-32 be") << QByteArray("\x00\x00\xFE\xFF", 4) + QByteArray(be32.encode(ohm)) << int(Encoding::Kind::Utf32BE) << true
                                   << ohm;
        const QString ascii = "part\tvalue\r\nR1\t1k\r\n";
        QTest::newRow("utf-16 le, no mark") << QByteArray(le.encode(ascii)) << int(Encoding::Kind::Utf16LE) << false << ascii;
        // A vendor's library: (c), degree, micro, euro, a curly quote and a
        // byte Windows-1252 leaves undefined (0x81).
        QTest::newRow("windows-1252") << QByteArray("* \xA9 2019 Vendor, T=25\xB0" "C, 10 \xB5" "A, 5 \x80, \x93q\x94 \x81\n")
                                      << int(Encoding::Kind::Windows1252) << false
                                      << QString::fromUtf8("* © 2019 Vendor, T=25°C, 10 µA, 5 €, “q” \u0081\n");
        QTest::newRow("empty") << QByteArray() << int(Encoding::Kind::Utf8) << false << QString();
        QTest::newRow("utf-8 cut short") << QByteArray("25 \xC2") << int(Encoding::Kind::Windows1252) << false << QString::fromUtf8("25 Â");
        QTest::newRow("utf-16, odd") << QByteArray("\xFF\xFE" "a" "\x00" "b", 5) << int(Encoding::Kind::Windows1252) << false
                                     << QString::fromUtf8("ÿþa") + QChar(0) + "b";
        QTest::newRow("utf-8 mark, then not utf-8") << QByteArray("\xEF\xBB\xBF" "25\xB0" "C\n") << int(Encoding::Kind::Windows1252) << false
                                                   << QString::fromUtf8("ï»¿25°C\n");
    }
    void encodingsAreFoundAndKept()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(int, kind);
        QFETCH(bool, bom);
        QFETCH(QString, text);
        Encoding found;
        QCOMPARE(qucs_s::textcodec::decode(bytes, &found), text);
        QCOMPARE(int(found.kind), kind);
        QCOMPARE(found.bom, bom);
        QByteArray back;
        QVERIFY(qucs_s::textcodec::encode(text, found, &back));
        QCOMPARE(back, bytes);
    }

    // Any bytes, written back as they were read - bytes not of the encoding
    // they seemed to be in (a UTF-8 mark and then no UTF-8) read as
    // Windows-1252, not changed to U+FFFD. Seeded.
    void anyBytesRoundTrip()
    {
        QRandomGenerator random(20260927);
        const QByteArray pieces[] = {"\xEF\xBB\xBF", "\xFF\xFE", "\xFE\xFF", QByteArray("\x00\x00", 2), "\xC3\xA9", "\xE2\x82\xAC", "\r\n", "a"};
        for (int n = 0; n < 4000; ++n) {
            QByteArray bytes;
            const int length = int(random.bounded(48));
            for (int i = 0; i < length; ++i) {
                if (random.bounded(4) == 0) bytes += pieces[random.bounded(int(std::size(pieces)))];
                else bytes += char(random.bounded(256));
            }
            Encoding found;
            const QString text = qucs_s::textcodec::decode(bytes, &found);
            QByteArray back;
            QVERIFY2(qucs_s::textcodec::encode(text, found, &back), bytes.toHex().constData());
            QVERIFY2(back == bytes, qPrintable(bytes.toHex() + " " + found.name()));
        }
    }

    // Windows-1252 has no Ω (nor any character beyond its 256): said, and
    // nothing written in its place.
    void whatAnEncodingLacksIsSaid()
    {
        const Encoding ansi{Encoding::Kind::Windows1252, false};
        QByteArray bytes = "x";
        QString missing;
        QVERIFY(!qucs_s::textcodec::encode(QString::fromUtf8("10 µA, 4.7 kΩ"), ansi, &bytes, &missing));
        QVERIFY(bytes.isEmpty());
        QCOMPARE(missing, QString::fromUtf8("Ω"));
        QVERIFY(!qucs_s::textcodec::encode(QString::fromUtf8("ok \U0001F600"), ansi, &bytes, &missing));
        QCOMPARE(missing, QString::fromUtf8("\U0001F600"));   // the pair, whole
        QVERIFY(qucs_s::textcodec::encode(QString::fromUtf8("10 µA, 5 €"), ansi, &bytes));
        QCOMPARE(bytes, QByteArray("10 \xB5" "A, 5 \x80"));
    }

    // Comment/Uncomment: the lines selected, as they are - the text before
    // the selection on its first line and after it on its last not made
    // twice (it was: "R1 *R1 a b 1k"); one step to undo.
    void commentedLinesAndNothingElse()
    {
        const QString file = dir.filePath("rc.cir");
        const QByteArray text = "R1 a b 1k\nC1 b 0 1n\nL1 b c 1u\n";
        QVERIFY(write(file, text));
        TextDoc doc(nullptr, file);
        QVERIFY(doc.load());

        select(doc, 3, 15);   // "a b 1k\nC1 b 0" - from after "R1 " into the second line
        doc.commentSelected();
        QCOMPARE(doc.toPlainText(), QString("*R1 a b 1k\n*C1 b 0 1n\nL1 b c 1u\n"));
        QCOMPARE(doc.textCursor().selectedText(), QString("*R1 a b 1k *C1 b 0 1n"));   // the lines, for another go
        doc.commentSelected();   // all comments: uncommented
        QCOMPARE(doc.toPlainText(), QString(text));
        doc.undo();
        QCOMPARE(doc.toPlainText(), QString("*R1 a b 1k\n*C1 b 0 1n\nL1 b c 1u\n"));   // one step each
        doc.undo();
        QCOMPARE(doc.toPlainText(), QString(text));

        // Lines selected the usual way, down to the start of the next: that
        // one is not taken.
        select(doc, 0, 20);   // to the start of "L1"
        doc.commentSelected();
        QCOMPARE(doc.toPlainText(), QString("*R1 a b 1k\n*C1 b 0 1n\nL1 b c 1u\n"));
        doc.undo();

        // A comment among the lines stays one (it was uncommented: a line
        // of the netlist).
        QVERIFY(write(file, "* bias\nR1 a b 1k\n\nC1 b 0 1n\n"));
        QVERIFY(doc.reload());
        select(doc, 0, doc.document()->characterCount() - 1);
        doc.commentSelected();
        QCOMPARE(doc.toPlainText(), QString("** bias\n*R1 a b 1k\n\n*C1 b 0 1n\n"));   // the blank line left
        doc.commentSelected();
        QCOMPARE(doc.toPlainText(), QString("* bias\nR1 a b 1k\n\nC1 b 0 1n\n"));

        // Indented comments, as Verilog-A has them.
        const QString va = dir.filePath("m.va");
        QVERIFY(write(va, "  // a\n  // b\n"));
        TextDoc v(nullptr, va);
        QVERIFY(v.load());
        select(v, 0, v.document()->characterCount() - 1);
        v.commentSelected();
        QCOMPARE(v.toPlainText(), QString("   a\n   b\n"));

        // No line comment (JSON): nothing.
        const QString json = dir.filePath("x.json");
        QVERIFY(write(json, "{\n  \"a\": 1\n}\n"));
        TextDoc j(nullptr, json);
        QVERIFY(j.load());
        select(j, 0, 8);
        j.commentSelected();
        QCOMPARE(j.toPlainText(), QString("{\n  \"a\": 1\n}\n"));
    }

    // The lines are the document's, not as laid out: a long line wrapped in
    // a narrow editor is one line (lines were counted as laid out).
    void wrappedLinesAreOneLine()
    {
        const QString file = dir.filePath("long.cir");
        const QString first = "R1 a b 1k " + QString(" x").repeated(200);
        QVERIFY(write(file, (first + "\nC1 b 0 1n\nL1 b c 1u\n").toUtf8()));
        TextDoc doc(nullptr, file);
        QVERIFY(doc.load());
        doc.setLineWrapMode(QPlainTextEdit::WidgetWidth);   // (wrapped, as the Markdown editor is)
        doc.setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        doc.resize(160, 400);
        doc.show();
        QVERIFY(QTest::qWaitForWindowExposed(&doc));
        // Laid out: the first line is several lines on the screen.
        doc.document()->documentLayout()->blockBoundingRect(doc.document()->firstBlock());
        QVERIFY2(doc.document()->firstBlock().lineCount() > 2, qPrintable(QString::number(doc.document()->firstBlock().lineCount())));
        const int second = doc.document()->findBlockByNumber(1).position();
        select(doc, second, second + 4);   // in "C1 b 0 1n"
        doc.commentSelected();
        QCOMPARE(doc.toPlainText(), first + "\n*C1 b 0 1n\nL1 b c 1u\n");
    }

    // Undo right after opening does not empty the document (loading was an
    // edit); nor after it is read again.
    void undoAfterOpeningKeepsTheFile()
    {
        const QString file = dir.filePath("keep.cir");
        QVERIFY(write(file, "R1 a b 1k\n"));
        TextDoc doc(nullptr, file);
        QVERIFY(doc.load());
        QVERIFY(!doc.document()->isUndoAvailable());
        doc.undo();
        QCOMPARE(doc.toPlainText(), QString("R1 a b 1k\n"));
        QVERIFY(!doc.getDocChanged());

        QVERIFY(write(file, "R1 a b 2k\n"));
        QVERIFY(doc.reload());
        QVERIFY(!doc.document()->isUndoAvailable());
        doc.undo();
        QCOMPARE(doc.toPlainText(), QString("R1 a b 2k\n"));

        // An edit is undone to the file as read.
        QTextCursor c(doc.document());
        c.movePosition(QTextCursor::End);
        c.insertText("C1 b 0 1n\n");
        doc.undo();
        QCOMPARE(doc.toPlainText(), QString("R1 a b 2k\n"));
    }

    // Saved in the encoding it was read in, its line ends as they were: a
    // Windows-1252 library keeps its °, µ, ©; a UTF-16 file stays UTF-16.
    void savedAsItWasRead()
    {
        const QString lib = dir.filePath("vendor.lib");
        const QByteArray ansi = "* \xA9 2019 Vendor, T=25\xB0" "C, 10 \xB5" "A\r\n.model D1 D(Is=1e-14)\r\n";
        QVERIFY(write(lib, ansi));
        TextDoc doc(nullptr, lib);
        QVERIFY(doc.load());
        QCOMPARE(doc.encoding().kind, Encoding::Kind::Windows1252);
        QVERIFY(doc.crlf());
        QVERIFY(doc.toPlainText().contains(QString::fromUtf8("T=25°C, 10 µA")));
        QCOMPARE(doc.save(), 0);   // as it is
        QCOMPARE(read(lib), ansi);
        QTextCursor c(doc.document());
        c.movePosition(QTextCursor::End);
        c.insertText(QString::fromUtf8(".param t=25 ; °C\n"));
        QCOMPARE(doc.save(), 0);
        QCOMPARE(read(lib), ansi + ".param t=25 ; \xB0" "C\r\n");

        const QString tsv = dir.filePath("table.txt");
        QStringEncoder le(QStringConverter::Utf16LE);
        const QByteArray utf16 = QByteArray("\xFF\xFE") + QByteArray(le.encode(QString::fromUtf8("f\tZ\n1e3\t50 Ω\n")));
        QVERIFY(write(tsv, utf16));
        TextDoc t(nullptr, tsv);
        QVERIFY(t.load());
        QCOMPARE(t.toPlainText(), QString::fromUtf8("f\tZ\n1e3\t50 Ω\n"));
        QCOMPARE(t.save(), 0);
        QCOMPARE(read(tsv), utf16);
        QVERIFY(t.writeTo(dir.filePath("copy.txt")));   // a copy (autosave's) the same
        QCOMPARE(read(dir.filePath("copy.txt")), utf16);
    }

    // A character the file's encoding has no bytes for (Ω in Windows-1252):
    // asked - no, nothing written, the edit kept; yes, the file becomes
    // UTF-8, every character in it. Claude's tools (no one to ask): UTF-8.
    void aCharacterItLacksIsAskedAbout()
    {
        const QString lib = dir.filePath("ohm.lib");
        const QByteArray ansi = "* 10 \xB5" "A\n";
        QVERIFY(write(lib, ansi));
        TextDoc doc(nullptr, lib);
        QVERIFY(doc.load());
        QTextCursor c(doc.document());
        c.movePosition(QTextCursor::End);
        c.insertText(QString::fromUtf8("* 4.7 kΩ\n"));
        QString asked;
        int result = 0;
        QVERIFY(answeringUtf8([&] { result = doc.save(); }, false, &asked));
        QCOMPARE(result, -1);
        QVERIFY2(asked.contains("Windows-1252") && asked.contains(QString::fromUtf8("Ω")), qPrintable(asked));
        QCOMPARE(read(lib), ansi);
        QVERIFY(doc.getDocChanged());
        QCOMPARE(doc.encoding().kind, Encoding::Kind::Windows1252);

        QVERIFY(answeringUtf8([&] { result = doc.save(); }, true));
        QCOMPARE(result, 0);
        QCOMPARE(read(lib), QString::fromUtf8("* 10 µA\n* 4.7 kΩ\n").toUtf8());
        QCOMPARE(doc.encoding().kind, Encoding::Kind::Utf8);
        QVERIFY(!doc.getDocChanged());

        // Claude's tools: no question.
        QVERIFY(write(lib, ansi));
        QVERIFY(doc.reload());
        QCOMPARE(doc.encoding().kind, Encoding::Kind::Windows1252);
        c = QTextCursor(doc.document());
        c.movePosition(QTextCursor::End);
        c.insertText(QString::fromUtf8("* Ω\n"));
        misc::ErrorCapture capture;
        QVERIFY(!answeringUtf8([&] { result = doc.save(); }, false));
        QCOMPARE(result, 0);
        QCOMPARE(read(lib), QString::fromUtf8("* 10 µA\n* Ω\n").toUtf8());
    }
};

QTEST_MAIN(TestTextEditing)
#include "test_text_editing.moc"
