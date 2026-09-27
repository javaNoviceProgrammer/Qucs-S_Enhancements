/*
 * A text document's settings file (name.cfg beside it: File > Document
 * Settings - VHDL and Verilog simulation, Verilog-A code creation) and the
 * setting that says whether saving writes it (QucsSettings.
 * WriteTextDocSettings, Application Settings > Settings): on, every save
 * writes it; off, only a document whose settings were set or changed
 * gets one, the settings read back as they were set, and a file already
 * there is left alone.
 */
#include <QtTest>
#include <QCheckBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "textdoc.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "settings.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

struct SettingGuard {
    ~SettingGuard() { QucsSettings.WriteTextDocSettings = true; }
};

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void write(const QString& path, const QByteArray& content)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

// A document written with a line of text and saved.
void saveNew(const QString& path)
{
    TextDoc doc(nullptr, path);
    doc.insertPlainText("-- a line\n");
    QCOMPARE(doc.save(), 0);
}

} // namespace

class TestTextDocSettings : public QObject
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
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void everySaveWritesItByDefault()
    {
        QVERIFY(_settings::Get().itemDefault<bool>("WriteTextDocSettings"));
        QVERIFY(QucsSettings.WriteTextDocSettings);
        const QString notes = dir.filePath("notes.txt");
        saveNew(notes);
        QVERIFY(QFile::exists(notes));
        QVERIFY(read(notes + ".cfg").startsWith("Textfile settings file, Qucs " PACKAGE_VERSION "\n"));
        QVERIFY(read(notes + ".cfg").contains("\nLibrary=\n"));
    }

    void offADocumentWithNewSettingsGetsNone()
    {
        SettingGuard guard;
        QucsSettings.WriteTextDocSettings = false;
        for (const char* name : {"plain.txt", "netlist.cir", "gate.vhdl", "count.v", "model.va", "readme.md"}) {
            const QString path = dir.filePath(name);
            saveNew(path);
            QVERIFY2(QFile::exists(path), name);
            QVERIFY2(!QFile::exists(path + ".cfg"), name);
        }
        // Opened, edited and saved again: still none.
        TextDoc doc(nullptr, dir.filePath("gate.vhdl"));
        QVERIFY(doc.load());
        doc.insertPlainText("-- another\n");
        QCOMPARE(doc.save(), 0);
        QVERIFY(!QFile::exists(dir.filePath("gate.vhdl.cfg")));
    }

    void offSettingsThatWereSetAreWrittenAndReadBack()
    {
        SettingGuard guard;
        QucsSettings.WriteTextDocSettings = false;
        const QString path = dir.filePath("adder.vhdl");
        {
            TextDoc doc(nullptr, path);
            doc.insertPlainText("-- an adder\n");
            // As Document Settings sets them (DigiSettingsDialog::slotOk).
            doc.simulation = false;
            doc.Library = "arith";
            doc.Libraries = "ieee";
            doc.SetChanged = true;
            QVERIFY(doc.writesSettings());
            QCOMPARE(doc.save(), 0);
            QVERIFY(!doc.SetChanged);
        }
        QVERIFY(read(path + ".cfg").contains("\nLibrary=arith\n"));
        {
            TextDoc doc(nullptr, path);
            QVERIFY(doc.load());
            QVERIFY(!doc.simulation);
            QCOMPARE(doc.Library, QString("arith"));
            QCOMPARE(doc.Libraries, QString("ieee"));
            // They are still not a new document's: saved again, written again.
            QVERIFY(doc.writesSettings());
            // Changed back to a new document's: written, so the file does
            // not keep the ones set before.
            doc.simulation = true;
            doc.Library.clear();
            doc.Libraries.clear();
            doc.SetChanged = true;
            QCOMPARE(doc.save(), 0);
            QVERIFY(!doc.writesSettings());
        }
        {
            TextDoc doc(nullptr, path);
            QVERIFY(doc.load());
            QVERIFY(doc.simulation);
            QVERIFY(doc.Library.isEmpty());
            QVERIFY(doc.Libraries.isEmpty());
        }
    }

    void offAFileAlreadyThereIsLeftAlone()
    {
        SettingGuard guard;
        QucsSettings.WriteTextDocSettings = false;
        const QString path = dir.filePath("old.txt");
        write(path, "old text\n");
        // Written by an older Qucs with a new document's settings.
        const QByteArray old = "Textfile settings file, Qucs 0.0.19\nSimulation=1\nDuration=\nModule=0\n"
                               "Library=\nLibraries=\nShortDesc=\nLongDesc=\nIcon=\nRecreate=0\n"
                               "DeviceType=512\n";
        write(path + ".cfg", old);
        TextDoc doc(nullptr, path);
        QVERIFY(doc.load());
        doc.insertPlainText("more\n");
        QCOMPARE(doc.save(), 0);
        QCOMPARE(read(path + ".cfg"), old);
        QVERIFY(read(path).contains("more"));

        // On: written again, as every save did.
        QucsSettings.WriteTextDocSettings = true;
        QCOMPARE(doc.save(), 0);
        QVERIFY(read(path + ".cfg").startsWith("Textfile settings file, Qucs " PACKAGE_VERSION "\n"));
    }

    void theSettingsDialogHasTheChoice()
    {
        SettingGuard guard;
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettings.WriteTextDocSettings = true;
        {
            QucsSettingsDialog dlg(&app);
            auto* box = dlg.findChild<QCheckBox*>("writeDocSettings");
            QVERIFY(box != nullptr);
            QVERIFY(box->isChecked());
            QVERIFY(box->toolTip().contains(".cfg"));
            box->setChecked(false);
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QVERIFY(!QucsSettings.WriteTextDocSettings);
            QVERIFY(!_settings::Get().item<bool>("WriteTextDocSettings"));
        }
        // Kept for the next start.
        QucsSettings.WriteTextDocSettings = true;
        QVERIFY(loadSettings());
        QVERIFY(!QucsSettings.WriteTextDocSettings);
        {
            QucsSettingsDialog dlg(&app);
            auto* box = dlg.findChild<QCheckBox*>("writeDocSettings");
            QVERIFY(!box->isChecked());   // as set
            auto* embed = dlg.findChild<QCheckBox*>("embedVerilogA");
            embed->setChecked(false);
            for (QPushButton* b : dlg.findChildren<QPushButton*>())
                if (b->text() == "Default Values") b->click();
            QVERIFY(box->isChecked());
            QVERIFY(embed->isChecked());
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QVERIFY(QucsSettings.WriteTextDocSettings);
        }
    }
};

QTEST_MAIN(TestTextDocSettings)
#include "test_text_doc_settings.moc"
