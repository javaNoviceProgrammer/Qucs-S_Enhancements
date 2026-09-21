/*
 * Regression tests for crashes taken from real macOS crash reports of the
 * 26.1.x binaries (see ENHANCEMENT_PROPOSAL.md, "Crash reports").
 *
 *  1. Saving a schematic whose symbol is defined by a Verilog-A file
 *     dereferenced a null Painting* in Schematic::adjustPortNumbers().
 *  2. Closing a text document tab crashed on macOS: destroying the
 *     syntax highlighter emitted textChanged() into the application,
 *     which looked up the "modified" tab label of a tab that no longer
 *     existed.
 */
#include <QtTest>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "textdoc.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "paintings/portsymbol.h"
#include "extsimkernels/spicecompat.h"

class TestCrashReports : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    static void write(const QString& path, const QString& text)
    {
        QFile f(path);
        QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Text), qPrintable(path));
        f.write(text.toUtf8());
    }

    // A schematic that is the symbol of a two-port Verilog-A module.
    void writeVerilogAProject(const QString& base)
    {
        write(dir.filePath(base + ".va"),
              "`include \"disciplines.vams\"\n"
              "module " + base + "(p, n);\n"
              "  inout p, n;\n"
              "  electrical p, n;\n"
              "  analog I(p, n) <+ V(p, n) / 1k;\n"
              "endmodule\n");
        write(dir.filePath(base + ".sch"),
              "<Qucs Schematic " PACKAGE_VERSION ">\n"
              "<Properties>\n"
              "  <View=0,0,800,600,1,0,0>\n"
              "  <Grid=10,10,1>\n"
              "  <DataSet=" + base + ".dat>\n"
              "  <DataDisplay=" + base + ".va>\n"
              "  <OpenDisplay=0>\n"
              "  <Script=" + base + ".m>\n"
              "  <RunScript=0>\n"
              "  <showFrame=0>\n"
              "  <FrameText0=Title>\n"
              "  <FrameText1=Drawn By:>\n"
              "  <FrameText2=Date:>\n"
              "  <FrameText3=Revision:>\n"
              "</Properties>\n"
              "<Symbol>\n"
              "  <.PortSym -40 0 1 0>\n"
              "  <.PortSym 40 0 2 180>\n"
              "  <.ID -20 24 X>\n"
              "</Symbol>\n"
              "<Components>\n</Components>\n<Wires>\n</Wires>\n"
              "<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;
        // QucsApp lists the simulators it can find and puts up a modal
        // error box when there is none: name one that exists.
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // Crash report qucs-s-2026-09-17-101148: Save All -> Schematic::save()
    // -> adjustPortNumbers() -> KERN_INVALID_ADDRESS at 0x50.
    void savingAVerilogASymbolSchematicDoesNotCrash()
    {
        writeVerilogAProject("vamod");
        Schematic sch(nullptr, dir.filePath("vamod.sch"));
        QVERIFY(sch.load());
        QCOMPARE(sch.a_SymbolPaints.size(), std::size_t(3));

        // What save() does first.
        QCOMPARE(sch.adjustPortNumbers(), 2);

        // Both port symbols were matched by number and named after the ports.
        QStringList names;
        for (auto* pp : sch.a_SymbolPaints)
            if (pp->Name == ".PortSym ")
                names << static_cast<PortSymbol*>(pp)->nameStr;
        names.sort();
        QCOMPARE(names, QStringList({"n", "p"}));

        // And the real thing: save the document (returns the port count,
        // or a negative value on failure).
        QVERIFY(sch.save() >= 0);
        QVERIFY(QFileInfo::exists(dir.filePath("vamod.sch")));
    }

    // Crash reports qucs-s-2026-09-17-085033 .. 094511 (8 of them):
    // TextDoc::~TextDoc -> ~SyntaxHighlighter -> textChanged ->
    // QucsApp::setDocumentTabChanged -> QLabel::setText on nullptr.
    void closingATextDocumentTabDoesNotCrash()
    {
        writeVerilogAProject("edit");
        const QString va = dir.filePath("edit.va");

        QucsApp app(false);
        QucsMain = &app;

        // Opening a file replaces the empty untitled document, so the tab
        // count stays at one; what matters is which document is current.
        app.editFile(va);
        auto* text = qobject_cast<TextDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(text != nullptr);
        QCOMPARE(text->getDocName(), va);

        // Make the document "modified", as an edited file would be: the
        // marker update on close is what used to blow up.
        text->insertPlainText("// touched\n");
        text->setDocChanged(false);              // ...but avoid the save prompt
        text->document()->setModified(false);

        app.slotFileClose();
        // The text document is gone and an untitled schematic took its place.
        QVERIFY(qobject_cast<TextDoc*>(app.DocumentTab->currentWidget()) == nullptr);
        QVERIFY(app.currentSchematic() != nullptr);
        QVERIFY(app.currentSchematic()->getDocName().isEmpty());

        // Several times in a row, and once with the text tab in the
        // background while a schematic is current.
        for (int i = 0; i < 3; ++i) {
            app.editFile(va);
            QVERIFY(qobject_cast<TextDoc*>(app.DocumentTab->currentWidget()) != nullptr);
            app.slotFileClose();
        }
        app.editFile(va);
        app.slotFileNew();                       // schematic tab becomes current
        QVERIFY(app.currentSchematic() != nullptr);
        QCOMPARE(app.DocumentTab->count(), 2);
        app.slotFileCloseAll();
        QCOMPARE(app.DocumentTab->count(), 1);   // only the fresh untitled one
        QucsMain = nullptr;
    }

    // The mechanism behind those reports, independent of which Qt version
    // happens to emit textChanged() while a highlighter is torn down: the
    // "modified" notification of a document arrives when that document has
    // no tab (it was just removed, or no tab is current at all). The old
    // code looked up the marker label of DocumentTab->currentIndex(), got
    // nullptr and called QLabel::setText() on it.
    void modifiedSignalFromADocumentWithoutATabIsIgnored()
    {
        writeVerilogAProject("orphan");
        QucsApp app(false);
        QucsMain = &app;

        app.editFile(dir.filePath("orphan.va"));
        auto* text = qobject_cast<TextDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(text != nullptr);
        const int index = app.DocumentTab->indexOf(text);

        // Exactly the state inside closeFile() between removeTab() and
        // delete: the document still exists but no tab refers to it.
        app.DocumentTab->removeTab(index);
        QCOMPARE(app.DocumentTab->currentIndex(), -1);
        text->slotSetChanged();                  // -> signalFileChanged -> app

        // A background document must not mark the current tab either.
        app.DocumentTab->addTab(text, "orphan.va");
        app.slotFileNew();
        QVERIFY(app.currentSchematic() != nullptr);
        text->slotSetChanged();

        app.slotFileCloseAll();
        QucsMain = nullptr;
    }
};

// Not QTEST_MAIN: QucsApp opens every command-line argument as a document,
// so QtTest's own arguments (function names, -v2, ...) must not reach the
// QApplication that QucsApp inspects.
int main(int argc, char** argv)
{
    int one = 1;
    QApplication app(one, argv);
    TestCrashReports test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_crash_reports.moc"
