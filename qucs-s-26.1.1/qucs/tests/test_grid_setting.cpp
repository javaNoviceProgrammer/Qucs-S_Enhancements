/*
 * The grid of every schematic as one setting (QucsSettings.GridMode,
 * Application Settings > Appearance > Schematic grid): as each schematic
 * says, always hidden or always shown - what is drawn, the files left as
 * they are, data displays keeping their own, View > Show Grid following
 * the setting, and the settings dialog.
 */
#include <QtTest>
#include <QComboBox>
#include <QImage>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
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

struct ModeGuard {
    ~ModeGuard() { QucsSettings.GridMode = 0; }
};

// Pixels of an empty schematic's canvas that are not paper: the grid.
int gridPixels(Schematic& doc)
{
    doc.resize(400, 300);
    doc.show();
    const QImage canvas = doc.viewport()->grab().toImage();
    const QColor paper = doc.viewport()->palette().color(doc.viewport()->backgroundRole());
    int n = 0;
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x)
            if (canvas.pixelColor(x, y) != paper) ++n;
    return n;
}

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestGridSetting : public QObject
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

    void theSettingDecidesWhatIsDrawn()
    {
        ModeGuard guard;
        Schematic on(nullptr, dir.filePath("on.sch"));
        Schematic off(nullptr, dir.filePath("off.sch"));
        off.setGridOn(false);

        QucsSettings.GridMode = 0;   // as each says
        QVERIFY(on.gridShown());
        QVERIFY(!off.gridShown());
        QVERIFY(gridPixels(on) > 0);
        QCOMPARE(gridPixels(off), 0);

        QucsSettings.GridMode = 1;   // always hidden
        QVERIFY(!on.gridShown());
        QCOMPARE(gridPixels(on), 0);

        QucsSettings.GridMode = 2;   // always shown
        QVERIFY(off.gridShown());
        QVERIFY(gridPixels(off) > 0);

        // The documents' own flags are what they were.
        QVERIFY(on.getGridOn());
        QVERIFY(!off.getGridOn());
    }

    // A data display has no grid by default and keeps its own flag.
    void aDataDisplayKeepsItsOwn()
    {
        ModeGuard guard;
        Schematic display(nullptr, dir.filePath("plots.dpl"));
        QVERIFY(!display.getGridOn());
        QucsSettings.GridMode = 2;
        QVERIFY(!display.gridShown());
        display.setGridOn(true);
        QucsSettings.GridMode = 1;
        QVERIFY(display.gridShown());
    }

    // Hidden everywhere, a schematic is saved with its own grid flag.
    void theFilesAreNotChanged()
    {
        ModeGuard guard;
        const QString path = dir.filePath("kept.sch");
        {
            Schematic doc(nullptr, path);
            QucsSettings.GridMode = 1;
            QVERIFY(doc.save() >= 0);
        }
        QVERIFY2(read(path).contains("<Grid=10,10,1>"), read(path).constData());
        QucsSettings.GridMode = 0;
        Schematic back(nullptr, path);
        QVERIFY(back.load());
        QVERIFY(back.getGridOn());
    }

    // View > Show Grid: the document's flag while each schematic has its
    // own; the setting for all schematics while it shows or hides them.
    void showGridFollowsTheSetting()
    {
        ModeGuard guard;
        QucsApp app(false);
        MainGuard main(&app);
        const QString path = dir.filePath("alt_g.sch");
        {
            Schematic doc(nullptr, path);
            QVERIFY(doc.save() >= 0);
        }
        QVERIFY(app.gotoPage(path));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);

        QucsSettings.GridMode = 0;
        app.updateGridAction();
        QVERIFY(app.showGrid->text().contains("current document"));
        QVERIFY(app.showGrid->isChecked());
        app.showGrid->trigger();
        QVERIFY(!doc->getGridOn());          // the document's own
        QVERIFY(doc->getDocChanged());
        QVERIFY(!app.showGrid->isChecked());
        app.showGrid->trigger();
        QVERIFY(doc->getGridOn());
        doc->setChanged(false);

        QucsSettings.GridMode = 1;
        app.applyGridSetting();
        QVERIFY(app.showGrid->text().contains("all schematics"));
        QVERIFY(!app.showGrid->isChecked());
        app.showGrid->trigger();             // every schematic's
        QCOMPARE(QucsSettings.GridMode, 2);
        QCOMPARE(_settings::Get().item<int>("GridMode"), 2);   // kept
        QVERIFY(app.showGrid->isChecked());
        QVERIFY(doc->getGridOn());
        QVERIFY(!doc->getDocChanged());      // the document is not touched
        app.showGrid->trigger();
        QCOMPARE(QucsSettings.GridMode, 1);
        QVERIFY(!doc->gridShown());
        app.closeAllFiles();
    }

    void theSettingsDialogHasTheChoice()
    {
        ModeGuard guard;
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettings.GridMode = 0;
        {
            QucsSettingsDialog dlg(&app);
            auto* combo = dlg.findChild<QComboBox*>("gridModeCombo");
            QVERIFY(combo != nullptr);
            QCOMPARE(combo->count(), 3);
            QCOMPARE(combo->currentData().toInt(), 0);
            combo->setCurrentIndex(combo->findData(1));
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QCOMPARE(QucsSettings.GridMode, 1);
            QCOMPARE(_settings::Get().item<int>("GridMode"), 1);
            QVERIFY(app.showGrid->text().contains("all schematics"));
        }
        {
            QucsSettingsDialog dlg(&app);
            auto* combo = dlg.findChild<QComboBox*>("gridModeCombo");
            QCOMPARE(combo->currentData().toInt(), 1);   // as set
            for (QPushButton* b : dlg.findChildren<QPushButton*>())
                if (b->text() == "Default Values") b->click();
            QCOMPARE(combo->currentData().toInt(), 0);
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QCOMPARE(QucsSettings.GridMode, 0);
        }
    }
};

QTEST_MAIN(TestGridSetting)
#include "test_grid_setting.moc"
