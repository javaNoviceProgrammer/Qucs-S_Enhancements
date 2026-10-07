/*
 * What the mouse wheel does on a schematic and a layout (QucsSettings.
 * WheelZooms, Application Settings > Settings > Mouse wheel): it zooms in
 * and out by default, Ctrl+wheel scrolling; set to scroll, the other way
 * round. Shift+wheel scrolls sideways either way, and a touchpad's swipe
 * scrolls whatever the setting (with Ctrl it zooms).
 */
#include <QtTest>
#include <QComboBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QWheelEvent>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "layoutdoc.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "settings.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using qucs_s::layout::LayoutView;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

struct SettingGuard {
    ~SettingGuard() { QucsSettings.WheelZooms = true; }
};

// A wheel's notch up at the middle of \a target: a mouse's (no phase, no
// pixels), or a touchpad's swipe (its phase and pixels).
void turn(QWidget* target, Qt::KeyboardModifiers modifiers, bool touchpad = false)
{
    const QPointF at(target->width() / 2.0, target->height() / 2.0);
    QWheelEvent event(at, target->mapToGlobal(at), touchpad ? QPoint(0, 30) : QPoint(), QPoint(0, 120), Qt::NoButton,
                      modifiers, touchpad ? Qt::ScrollUpdate : Qt::NoScrollPhase, false);
    QApplication::sendEvent(target, &event);
}

// What a turn did to the schematic: zoomed, or scrolled (the model point
// at the top left moved, the scale the same).
enum class Did { Zoomed, Scrolled, ScrolledSideways, Nothing };

Did turned(Schematic* sch, Qt::KeyboardModifiers modifiers, bool touchpad = false)
{
    const double scale = sch->getScale();
    const QPoint corner = sch->viewportToModel(QPoint(0, 0));
    // (To the scroll view: Qt passes a wheel turn the viewport leaves on
    // to it, but only a real one.)
    turn(sch, modifiers, touchpad);
    if (!qFuzzyCompare(sch->getScale(), scale)) return Did::Zoomed;
    const QPoint now = sch->viewportToModel(QPoint(0, 0));
    if (now.y() != corner.y()) return Did::Scrolled;
    if (now.x() != corner.x()) return Did::ScrolledSideways;
    return Did::Nothing;
}

Did turned(LayoutView* view, Qt::KeyboardModifiers modifiers, bool touchpad = false)
{
    view->setView(QPointF(0, 0), 10);
    turn(view, modifiers, touchpad);
    if (!qFuzzyCompare(view->scale(), 10.0)) return Did::Zoomed;
    if (view->center().y() != 0) return Did::Scrolled;
    if (view->center().x() != 0) return Did::ScrolledSideways;
    return Did::Nothing;
}

} // namespace

namespace QTest {
template <> char* toString(const Did& did)
{
    const char* names[] = {"Zoomed", "Scrolled", "ScrolledSideways", "Nothing"};
    return qstrdup(names[int(did)]);
}
} // namespace QTest

class TestMouseWheel : public QObject
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

    // Zoom by default; set to scroll, the other way round.
    void aSchematicZoomsOrScrollsAsSet()
    {
        SettingGuard guard;
        QCOMPARE(_settings::Get().itemDefault<bool>("WheelZooms"), true);
        QucsApp app(false);
        MainGuard main(&app);
        app.resize(1000, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        if (app.currentSchematic() == nullptr) app.slotFileNew();
        Schematic* sch = app.currentSchematic();
        QVERIFY(sch != nullptr);

        QucsSettings.WheelZooms = true;
        QCOMPARE(turned(sch, Qt::NoModifier), Did::Zoomed);
        QCOMPARE(turned(sch, Qt::ControlModifier), Did::Scrolled);
        QCOMPARE(turned(sch, Qt::ShiftModifier), Did::ScrolledSideways);
        // A touchpad's swipe scrolls, with Ctrl zooms.
        QCOMPARE(turned(sch, Qt::NoModifier, true), Did::Scrolled);
        QCOMPARE(turned(sch, Qt::ControlModifier, true), Did::Zoomed);

        QucsSettings.WheelZooms = false;
        QCOMPARE(turned(sch, Qt::NoModifier), Did::Scrolled);
        QCOMPARE(turned(sch, Qt::ControlModifier), Did::Zoomed);
        QCOMPARE(turned(sch, Qt::ShiftModifier), Did::ScrolledSideways);
        QCOMPARE(turned(sch, Qt::NoModifier, true), Did::Scrolled);
        QCOMPARE(turned(sch, Qt::ControlModifier, true), Did::Zoomed);
    }

    // A layout the same; a trackpad pans it.
    void aLayoutZoomsOrPansAsSet()
    {
        SettingGuard guard;
        LayoutView view;
        view.resize(400, 300);
        QucsSettings.WheelZooms = true;
        QCOMPARE(turned(&view, Qt::NoModifier), Did::Zoomed);
        QCOMPARE(turned(&view, Qt::ControlModifier), Did::Scrolled);
        QCOMPARE(turned(&view, Qt::ShiftModifier), Did::ScrolledSideways);
        QCOMPARE(turned(&view, Qt::NoModifier, true), Did::Scrolled);
        QCOMPARE(turned(&view, Qt::ControlModifier, true), Did::Zoomed);

        QucsSettings.WheelZooms = false;
        QCOMPARE(turned(&view, Qt::NoModifier), Did::Scrolled);
        QCOMPARE(turned(&view, Qt::ControlModifier), Did::Zoomed);
        QCOMPARE(turned(&view, Qt::ShiftModifier), Did::ScrolledSideways);
        QCOMPARE(turned(&view, Qt::NoModifier, true), Did::Scrolled);
    }

    // The choice in the dialog: applied, kept for the next start, and
    // zoom again with Default Values.
    void theSettingsDialogHasTheChoice()
    {
        SettingGuard guard;
        QucsApp app(false);
        MainGuard main(&app);
        QucsSettings.WheelZooms = true;
        {
            QucsSettingsDialog dlg(&app);
            auto* combo = dlg.findChild<QComboBox*>("wheelCombo");
            QVERIFY(combo != nullptr);
            QCOMPARE(combo->currentText(), QString("Zooms in and out"));
            QVERIFY(combo->toolTip().contains("Ctrl+wheel"));
            combo->setCurrentIndex(combo->findData(false));
            QCOMPARE(combo->currentText(), QString("Scrolls"));
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QVERIFY(!QucsSettings.WheelZooms);
            QVERIFY(!_settings::Get().item<bool>("WheelZooms"));
        }
        QucsSettings.WheelZooms = true;
        QVERIFY(loadSettings());
        QVERIFY(!QucsSettings.WheelZooms);
        {
            QucsSettingsDialog dlg(&app);
            auto* combo = dlg.findChild<QComboBox*>("wheelCombo");
            QCOMPARE(combo->currentText(), QString("Scrolls"));   // as set
            for (QPushButton* b : dlg.findChildren<QPushButton*>())
                if (b->text() == "Default Values") b->click();
            QCOMPARE(combo->currentText(), QString("Zooms in and out"));
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QVERIFY(QucsSettings.WheelZooms);
        }
    }
};

QTEST_MAIN(TestMouseWheel)
#include "test_mouse_wheel.moc"
