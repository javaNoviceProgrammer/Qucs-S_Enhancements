/*
 * The Theme setting: System, Dark or Light for the application's
 * windows, menus and dialogs. On the offscreen platform Qt cannot switch
 * the appearance, so this exercises the palette the setting falls back
 * to, the settings dialog's control and the setting's storage.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QComboBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyleHints>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "apptheme.h"
#include "settings.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "dialogs/qucssettingsdialog.h"
#include "textdoc.h"

using namespace qucs_s::apptheme;

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The setting and the look are global: put both back whatever happens.
struct ThemeGuard {
    int theme = QucsSettings.Theme;
    ~ThemeGuard() { QucsSettings.Theme = theme; apply(System); }
};

QComboBox* themeCombo(QWidget* dialog)
{
    for (QComboBox* c : dialog->findChildren<QComboBox*>())
        if (c->count() == 3 && c->itemData(1).toInt() == Dark && c->itemData(2).toInt() == Light) return c;
    return nullptr;
}

QPushButton* button(QWidget* w, const QString& text)
{
    for (QPushButton* b : w->findChildren<QPushButton*>())
        if (b->text() == text) return b;
    return nullptr;
}
} // namespace

class TestAppTheme : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QColor systemWindow;   // the window colour the platform gave us
    bool systemIsDark = false;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.Theme = System;
        QucsSettings.BGColor = QColor(255, 250, 225);   // the document background, as shipped
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        systemWindow = QApplication::palette().color(QPalette::Window);
        systemIsDark = isDark();
    }

    void storedValuesAreOneOfThree()
    {
        QCOMPARE(bounded(System), int(System));
        QCOMPARE(bounded(Dark), int(Dark));
        QCOMPARE(bounded(Light), int(Light));
        QCOMPARE(bounded(-1), int(System));
        QCOMPARE(bounded(3), int(System));
        QCOMPARE(bounded(1234), int(System));
    }

    void theFallbackPalettesAreReadable()
    {
        // What a platform that cannot switch its appearance gets.
        const QPalette dark = darkPalette();
        QVERIFY(dark.color(QPalette::Window).value() < 0x60);
        QVERIFY(dark.color(QPalette::Base).value() < 0x60);
        QVERIFY(dark.color(QPalette::Text).value() > 0xc0);
        QVERIFY(dark.color(QPalette::WindowText).value() > 0xc0);
        QVERIFY(dark.color(QPalette::ButtonText).value() > 0xc0);
        QVERIFY(dark.color(QPalette::HighlightedText).value() > 0xc0);
        // Disabled text is dimmer than enabled text, still readable.
        QVERIFY(dark.color(QPalette::Disabled, QPalette::Text).value() < dark.color(QPalette::Text).value());
        QVERIFY(dark.color(QPalette::Disabled, QPalette::Text).value() > dark.color(QPalette::Window).value() + 0x30);
        // Every role is the palette's own: nothing of the platform's shows through.
        QVERIFY(dark.isBrushSet(QPalette::Active, QPalette::Window));
        QVERIFY(dark.isBrushSet(QPalette::Disabled, QPalette::Text));

        const QPalette light = lightPalette();
        QVERIFY(light.color(QPalette::Window).value() > 0xc0);
        QVERIFY(light.color(QPalette::Base).value() > 0xe0);
        QVERIFY(light.color(QPalette::Text).value() < 0x40);
        QVERIFY(light.color(QPalette::WindowText).value() < 0x40);
    }

    void darkLightAndBackToTheSystem()
    {
        ThemeGuard guard;
        apply(Dark);
        QVERIFY(isDark());
        QVERIFY(misc::isDarkTheme());

        apply(Light);
        QVERIFY(!isDark());
        const QPalette light = QApplication::palette();

        // Applying the same theme again changes nothing.
        apply(Light);
        QCOMPARE(QApplication::palette(), light);

        apply(System);
        QCOMPARE(QApplication::palette().color(QPalette::Window), systemWindow);
        // Nothing of ours is left on the palette: the platform's colours
        // come through again, now and when the system changes them.
        QCOMPARE(QApplication::palette().resolveMask(), QPalette::ResolveMask(0));
        QVERIFY(!QCoreApplication::testAttribute(Qt::AA_SetPalette));

        // An unknown value is the system's look.
        apply(Dark);
        apply(42);
        QCOMPARE(QApplication::palette().color(QPalette::Window), systemWindow);
    }

    void whereThePlatformCannotSwitchThePaletteDoesIt()
    {
        // The offscreen platform answers no request for a colour scheme,
        // so the palette is the way; where it does (Cocoa, Windows) no
        // palette is forced and the native controls follow instead.
        ThemeGuard guard;
        apply(Dark);
        const bool platformSwitched = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        if (platformSwitched)
            QVERIFY(!QCoreApplication::testAttribute(Qt::AA_SetPalette));
        else
            QVERIFY(QCoreApplication::testAttribute(Qt::AA_SetPalette));
        QVERIFY(isDark());
    }

    void theSettingIsLoadedBoundedAndSaved()
    {
        ThemeGuard guard;
        QCOMPARE(_settings::Get().itemDefault<int>("Theme"), int(System));
        _settings::Get().setItem<int>("Theme", 7);
        QCOMPARE(bounded(_settings::Get().item<int>("Theme")), int(System));   // as loadSettings() does
        QucsSettings.Theme = Dark;
        saveApplSettings();
        QCOMPARE(_settings::Get().item<int>("Theme"), int(Dark));
    }

    void theTextEditorKeepsItsBackgroundInTheDarkTheme()
    {
        // The editor shows the document background from the settings,
        // whatever the theme - like the schematic. (The main window has a
        // style sheet, and the style-sheet style used to put the theme's
        // base colour back on the editor each time it was shown.)
        ThemeGuard guard;
        QucsApp app(false);
        MainGuard mainGuard(&app);
        app.resize(900, 600);
        app.show();
        const QString file = dir.filePath("colours.txt");
        { QFile f(file); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("line one\nline two\n"); }
        QVERIFY(app.gotoPage(file, false, false));
        TextDoc* doc = qobject_cast<TextDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        auto background = [&] {
            QCoreApplication::processEvents();
            const QImage img = doc->viewport()->grab().toImage();
            return img.pixelColor(img.width() - 4, img.height() - 4);   // below the last line
        };
        const QColor paper = QucsSettings.BGColor;
        QCOMPARE(background(), paper);
        apply(Dark);
        QTest::qWait(50);
        QVERIFY(isDark());
        QCOMPARE(background(), paper);
        // Dark text on the light paper, not the theme's light text.
        const QImage img = doc->viewport()->grab().toImage();
        int darkPixels = 0;
        for (int y = 0; y < qMin(img.height(), 20); ++y)
            for (int x = 0; x < qMin(img.width(), 80); ++x)
                if (img.pixelColor(x, y).value() < 0x40) ++darkPixels;
        QVERIFY(darkPixels > 0);
        // A new document background from the settings reaches an open editor.
        struct BgGuard { QColor c = QucsSettings.BGColor; ~BgGuard() { QucsSettings.BGColor = c; } } bgGuard;
        QucsSettings.BGColor = QColor(0x20, 0x20, 0x40);
        doc->applyDocumentColors();
        QCOMPARE(background(), QColor(0x20, 0x20, 0x40));
        apply(System);
        QTest::qWait(50);
        QCOMPARE(background(), QColor(0x20, 0x20, 0x40));
    }

    void theSettingsDialogHasTheThemeChoice()
    {
        ThemeGuard guard;
        QucsApp app(false);
        MainGuard mainGuard(&app);
        {
            QucsSettingsDialog dlg(&app);
            QComboBox* combo = themeCombo(&dlg);
            QVERIFY(combo != nullptr);
            QCOMPARE(combo->itemText(0), QString("System"));
            QCOMPARE(combo->itemText(1), QString("Dark"));
            QCOMPARE(combo->itemText(2), QString("Light"));
            QCOMPARE(combo->currentData().toInt(), int(System));   // what is set now
            combo->setCurrentIndex(1);
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QCOMPARE(QucsSettings.Theme, int(Dark));
            QCOMPARE(_settings::Get().item<int>("Theme"), int(Dark));   // saved
            QVERIFY(isDark());
            QVERIFY(QucsSettings.hasDarkTheme);
            // The windows follow: Qt posts them the palette change.
            QTRY_VERIFY(app.palette().color(QPalette::Window).value() < 0x60);
        }
        {
            // Opened again it shows the choice made; Defaults puts System back.
            QucsSettingsDialog dlg(&app);
            QComboBox* combo = themeCombo(&dlg);
            QVERIFY(combo != nullptr);
            QCOMPARE(combo->currentData().toInt(), int(Dark));
            QPushButton* defaults = button(&dlg, "Default Values");
            QVERIFY(defaults != nullptr);
            defaults->click();
            QCOMPARE(combo->currentData().toInt(), int(System));
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QCOMPARE(QucsSettings.Theme, int(System));
            QCOMPARE(isDark(), systemIsDark);
            QCOMPARE(QucsSettings.hasDarkTheme, systemIsDark);
            QTRY_COMPARE(app.palette().color(QPalette::Window), systemWindow);
        }
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestAppTheme test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_app_theme.moc"
