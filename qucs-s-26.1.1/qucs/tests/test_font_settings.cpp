/*
 * The three font buttons of Application Settings > Appearance: what is picked
 * reaches the button, and what the button says is what Apply saves and Cancel
 * drops.
 *
 * On macOS these play what the real font panel does (a QFontDialog with no
 * buttons, told a font at each change and once more as it closes, then
 * rejected: seen on macOS 27 with Qt 6.12, docs/feature_gaps/
 * 2026-11-07-font-panel.md). The panel itself needs a display: the driver in
 * that write-up's folder works it. Elsewhere they use Qt's own dialog, with its
 * OK and Cancel.
 */
#include <QtTest>
#include <QFontDatabase>
#include <QFontDialog>
#include <QFontInfo>
#include <QGridLayout>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <functional>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "misc.h"
#include "main.h"
#include "settings.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// As the settings say, and as they were after the test.
struct FontsGuard {
    const QFont font = QucsSettings.font;
    const QFont appFont = QucsSettings.appFont;
    const QFont textFont = QucsSettings.textFont;
    ~FontsGuard()
    {
        QucsSettings.font = font;
        QucsSettings.appFont = appFont;
        QucsSettings.textFont = textFont;
    }
};

QPushButton* fontButton(QucsSettingsDialog& dlg, int row)
{
    for (QTabWidget* tabs : dlg.findChildren<QTabWidget*>())
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i).remove('&') == "Appearance") {
                auto* grid = tabs->widget(i)->findChild<QGridLayout*>();
                return grid != nullptr ? qobject_cast<QPushButton*>(grid->itemAtPosition(row, 1)->widget()) : nullptr;
            }
    return nullptr;
}

// Presses the button; \a inTheDialog runs while its font dialog is up. False if
// no font dialog came.
bool pressAndWork(QPushButton* button, const std::function<void(QFontDialog*)>& inTheDialog)
{
    bool seen = false;
    QTimer::singleShot(0, button, [&] {
        auto* dialog = qobject_cast<QFontDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) return;
        seen = true;
        inTheDialog(dialog);
    });
    button->click();
    return seen;
}

// A family to pick. macOS's panel is told one (Menlo, there on every Mac).
// Qt's own dialog picks only among the families it lists: one it lacks comes
// back as the nearest it has - Menlo as "Sans Serif" on CI's Linux, where the
// tests asked for Menlo and saw that. So there, a family of its list: the
// fixed-pitch font's if listed, else the first scalable one.
QString aFamily()
{
#ifdef Q_OS_MACOS
    return QStringLiteral("Menlo");
#else
    const QStringList families = QFontDatabase::families();
    const QString fixed = QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family();
    if (families.contains(fixed) && QFontDatabase::isSmoothlyScalable(fixed)) return fixed;
    for (const QString& f : families)
        if (!QFontDatabase::isPrivateFamily(f) && QFontDatabase::isSmoothlyScalable(f) && !f.contains(u'[')) return f;
    return {};
#endif
}

QFont& setting(int row)
{
    return row == 0 ? QucsSettings.font : row == 1 ? QucsSettings.appFont : QucsSettings.textFont;
}

} // namespace

class TestFontSettings : public QObject
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

    void aPickedFontReachesTheButtonAndApplySavesIt_data()
    {
        QTest::addColumn<int>("row");
        QTest::newRow("schematic") << 0;
        QTest::newRow("application") << 1;
        QTest::newRow("text document") << 2;
    }
    void aPickedFontReachesTheButtonAndApplySavesIt()
    {
        QFETCH(int, row);
        const QString family = aFamily();
        if (family.isEmpty()) QSKIP("No scalable font family here to pick.");
        FontsGuard guard;
        setting(row) = QFont(family, 10);
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettingsDialog dlg(&app);
        QPushButton* button = fontButton(dlg, row);
        QVERIFY(button != nullptr);
        QVERIFY(button->text().startsWith(family + ",10"));

        const QFont fourteen(family, 14);
        QVERIFY(pressAndWork(button, [&](QFontDialog* dialog) {
#ifdef Q_OS_MACOS
            QVERIFY(dialog->testOption(QFontDialog::NoButtons));   // the panel's own form
            emit dialog->currentFontChanged(fourteen);             // the user's change
            emit dialog->currentFontChanged(fourteen);             // and as it closes
            dialog->reject();                                      // the red button
#else
            dialog->setCurrentFont(fourteen);
            dialog->accept();
#endif
        }));
        QCOMPARE(button->text().section(u',', 0, 1), family + ",14");   // (then a style, if the dialog names one)
        QCOMPARE(setting(row).pointSize(), 10);                    // not before Apply

        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QCOMPARE(setting(row).family(), family);
        QCOMPARE(setting(row).pointSize(), 14);
    }

    // The settings' Cancel is what undoes it.
    void cancelInTheSettingsDropsIt()
    {
        const QString family = aFamily();
        if (family.isEmpty()) QSKIP("No scalable font family here to pick.");
        FontsGuard guard;
        QucsSettings.textFont = QFont(family, 10);
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettingsDialog dlg(&app);
        QPushButton* button = fontButton(dlg, 2);
        QVERIFY(button != nullptr);
        const QFont fourteen(family, 14);
        QVERIFY(pressAndWork(button, [&](QFontDialog* dialog) {
#ifdef Q_OS_MACOS
            emit dialog->currentFontChanged(fourteen);
            emit dialog->currentFontChanged(fourteen);
            dialog->reject();
#else
            dialog->setCurrentFont(fourteen);
            dialog->accept();
#endif
        }));
        QCOMPARE(button->text().section(u',', 0, 1), family + ",14");   // (then a style, if the dialog names one)
        dlg.reject();
        QCOMPARE(QucsSettings.textFont.pointSize(), 10);
    }

    // Opened and closed with nothing chosen, nothing changes - and closing
    // tells a font, which on a font the panel cannot show is another.
    void closingWithNothingChosenChangesNothing()
    {
        FontsGuard guard;
        QFont half("Menlo");
        half.setPointSizeF(10.5);
        QucsSettings.textFont = half;
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettingsDialog dlg(&app);
        QPushButton* button = fontButton(dlg, 2);
        QVERIFY(button != nullptr);
        const QString before = button->text();
        QVERIFY(pressAndWork(button, [&](QFontDialog* dialog) {
#ifdef Q_OS_MACOS
            emit dialog->currentFontChanged(QFont("Academy Engraved LET", 11));   // as it closes
#endif
            dialog->reject();
        }));
        QCOMPARE(button->text(), before);
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QCOMPARE(QucsSettings.textFont.pointSizeF(), 10.5);
        QCOMPARE(QucsSettings.textFont.family(), QString("Menlo"));
    }

#ifdef Q_OS_MACOS
    // The system's own font is not in the panel's list. A new size keeps the
    // font; another family in the list is taken.
    void theSystemFontKeepsItselfThroughANewSize()
    {
        FontsGuard guard;
        QFont system(".AppleSystemUIFont", 13);
        QucsSettings.appFont = system;
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettingsDialog dlg(&app);
        QPushButton* button = fontButton(dlg, 1);
        QVERIFY(button != nullptr);

        QFont opened;
        QVERIFY(pressAndWork(button, [&](QFontDialog* dialog) {
            opened = dialog->currentFont();
            QFont sized(opened.family(), 14);
            sized.setStyleName("Regular");
            emit dialog->currentFontChanged(sized);
            emit dialog->currentFontChanged(sized);
            dialog->reject();
        }));
        QVERIFY(!opened.family().startsWith('.'));   // not one the panel lacks
        QCOMPARE(button->text(), QString(".AppleSystemUIFont,14"));

        QVERIFY(pressAndWork(button, [&](QFontDialog* dialog) {
            const QFont other("Menlo", 14);
            emit dialog->currentFontChanged(other);
            emit dialog->currentFontChanged(other);
            dialog->reject();
        }));
        QCOMPARE(button->text(), QString("Menlo,14"));
    }

    // While the tools hold the native dialogs back it is Qt's own, which has
    // buttons, and they mean what they say.
    void qtsOwnDialogKeepsItsButtons()
    {
        FontsGuard guard;
        QucsSettings.textFont = QFont("Menlo", 10);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        auto restore = qScopeGuard([] { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false); });
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettingsDialog dlg(&app);
        QPushButton* button = fontButton(dlg, 2);
        QVERIFY(button != nullptr);
        QVERIFY(pressAndWork(button, [&](QFontDialog* dialog) {
            QVERIFY(!dialog->testOption(QFontDialog::NoButtons));
            dialog->setCurrentFont(QFont("Menlo", 14));
            dialog->reject();
        }));
        QVERIFY(button->text().startsWith("Menlo,10"));
    }
#endif
};

QTEST_MAIN(TestFontSettings)
#include "test_font_settings.moc"
