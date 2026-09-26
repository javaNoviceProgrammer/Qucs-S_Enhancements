/*
 * The toolbars locked in place (View > Toolbars > Lock Toolbars, the
 * menu of a right click on them, Application Settings > Appearance): no
 * handle, no drag moves one or takes it off the window, one floating
 * comes back; unlocked, they are arranged as before. The lock is kept in
 * the settings.
 */
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolBar>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "settings.h"
#include "qucsshortcutmanager.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

struct LockGuard {
    ~LockGuard() { QucsSettings.LockToolbars = false; }
};

// A mouse event for \a w at \a global, a point of the screen.
void send(QWidget* w, QEvent::Type type, QPoint global, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, w->mapFromGlobal(QPointF(global)), QPointF(global),
                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

// A drag of \a bar from its handle - left of its first button, where the
// handle is when it has one - to \a to, a point of \a window.
void drag(QToolBar* bar, QWidget* window, QPoint to)
{
    const QWidget* first = bar->widgetForAction(bar->actions().constFirst());
    const QPoint start = bar->mapToGlobal(QPoint(std::max(1, first->x() / 2), bar->height() / 2));
    const QPoint end = window->mapToGlobal(to);
    send(bar, QEvent::MouseButtonPress, start, Qt::LeftButton);
    for (int step = 1; step <= 12; ++step) {
        send(bar, QEvent::MouseMove, start + (end - start) * step / 12, Qt::LeftButton);
        QCoreApplication::processEvents();
    }
    send(bar, QEvent::MouseButtonRelease, end, Qt::NoButton);
    QCoreApplication::processEvents();
}

// A menu's submenu (of the menu bar's when \a in is null) by its title.
QMenu* submenu(QucsApp& app, QMenu* in, const QString& title)
{
    const QList<QAction*> actions = in != nullptr ? in->actions() : app.menuBar()->actions();
    for (QAction* a : actions)
        if (a->menu() != nullptr && a->text().remove('&') == title) return a->menu();
    return nullptr;
}

QMenu* toolbarsMenu(QucsApp& app)
{
    QMenu* view = submenu(app, nullptr, QStringLiteral("View"));
    return view != nullptr ? submenu(app, view, QStringLiteral("Toolbars")) : nullptr;
}

} // namespace

class TestToolbarLock : public QObject
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

    // Unlocked (as always before), a toolbar is dragged elsewhere; locked,
    // none moves and their handles go; unlocked again, they move.
    void aLockedToolbarIsNotMoved()
    {
        LockGuard guard;
        QucsApp app(false);
        MainGuard main(&app);
        app.resize(1100, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));

        QCOMPARE(app.toolbars().size(), 6);
        QAction* lock = app.findChild<QAction*>(QStringLiteral("lockToolbars"));
        QVERIFY(lock != nullptr);
        QVERIFY(lock->isCheckable());
        QVERIFY(!lock->isChecked());
        QVERIFY(!app.toolbarsLocked());
        for (QToolBar* bar : app.toolbars()) {
            QVERIFY(bar != nullptr);
            QVERIFY(!bar->objectName().isEmpty());
            QVERIFY(bar->isMovable());
        }
        // A shortcut can be given to it (the manager is the process's: the
        // first window's commands).
        QucsCommand* command = QucsShortcutManager::instance().command(QStringLiteral("View.LockToolbars"));
        QVERIFY(command != nullptr);
        QCOMPARE(command->action(), lock);

        // Unlocked: the Work toolbar dragged to the window's bottom.
        QToolBar* work = app.findChild<QToolBar*>(QStringLiteral("workToolbar"));
        QVERIFY(work != nullptr);
        QCOMPARE(app.toolBarArea(work), Qt::TopToolBarArea);
        const int bottom = app.statusBar()->geometry().top() - 12;
        drag(work, &app, QPoint(app.width() / 2, bottom));
        QTRY_COMPARE(app.toolBarArea(work), Qt::BottomToolBarArea);

        // Locked: it stays where it is, as do the others.
        lock->trigger();
        QVERIFY(app.toolbarsLocked());
        QVERIFY(lock->isChecked());
        for (QToolBar* bar : app.toolbars()) QVERIFY(!bar->isMovable());
        QCOMPARE(_settings::Get().item<bool>("LockToolbars"), true);
        QTest::qWait(50);   // (laid out again without the handles)
        const QRect before = work->geometry();
        drag(work, &app, QPoint(app.width() / 2, 60));
        QTest::qWait(50);
        QCOMPARE(app.toolBarArea(work), Qt::BottomToolBarArea);
        QCOMPARE(work->geometry(), before);
        QToolBar* file = app.findChild<QToolBar*>(QStringLiteral("fileToolbar"));
        const QRect fileBefore = file->geometry();
        drag(file, &app, QPoint(app.width() / 2, bottom));
        QCOMPARE(app.toolBarArea(file), Qt::TopToolBarArea);
        QCOMPARE(file->geometry(), fileBefore);

        // Unlocked again: it moves back.
        lock->trigger();
        QVERIFY(!app.toolbarsLocked());
        for (QToolBar* bar : app.toolbars()) QVERIFY(bar->isMovable());
        QCOMPARE(_settings::Get().item<bool>("LockToolbars"), false);
        QTest::qWait(50);   // (their handles back)
        drag(work, &app, QPoint(app.width() / 2, 60));
        QTRY_COMPARE(app.toolBarArea(work), Qt::TopToolBarArea);
    }

    // A toolbar dragged off the window floats; locking brings it back (it
    // would have no handle to be brought back by).
    void aFloatingToolbarComesBackWhenLocked()
    {
        LockGuard guard;
        QucsApp app(false);
        MainGuard main(&app);
        app.resize(1100, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QToolBar* view = app.findChild<QToolBar*>(QStringLiteral("viewToolbar"));
        drag(view, &app, QPoint(app.width() / 2, app.height() / 2));
        if (!view->isFloating()) QSKIP("the platform does not float a toolbar dragged off its window");
        app.setToolbarsLocked(true);
        QVERIFY(!view->isFloating());
        QVERIFY(!view->isMovable());
        QVERIFY(app.toolBarArea(view) != Qt::NoToolBarArea);
        QTRY_VERIFY(view->isVisible());   // (shown again a moment later)
        QVERIFY(!view->isWindow());
        QVERIFY(app.geometry().contains(view->mapTo(&app, QPoint()) + app.geometry().topLeft()));
    }

    // View > Toolbars: each toolbar shown or hidden, and Lock Toolbars; the
    // right click on a toolbar offers the lock too.
    void theLockIsInTheMenus()
    {
        LockGuard guard;
        QucsApp app(false);
        MainGuard main(&app);
        app.resize(1100, 700);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        QAction* lock = app.findChild<QAction*>(QStringLiteral("lockToolbars"));

        QMenu* menu = toolbarsMenu(app);
        QVERIFY(menu != nullptr);
        QList<QAction*> items;
        for (QAction* a : menu->actions())
            if (!a->isSeparator()) items << a;
        QCOMPARE(items.size(), 7);
        for (int i = 0; i < 6; ++i) QCOMPARE(items.at(i), app.toolbars().at(i)->toggleViewAction());
        QCOMPARE(items.last(), lock);
        // A toolbar hidden and shown again from there.
        QToolBar* simulate = app.findChild<QToolBar*>(QStringLiteral("simulateToolbar"));
        simulate->toggleViewAction()->trigger();
        QVERIFY(simulate->isHidden());
        simulate->toggleViewAction()->trigger();
        QVERIFY(!simulate->isHidden());

        // The right click on a toolbar: its menu has the lock, and locks.
        QToolBar* edit = app.findChild<QToolBar*>(QStringLiteral("editToolbar"));
        const QPoint at(edit->width() - 3, edit->height() / 2);
        QContextMenuEvent right(QContextMenuEvent::Mouse, at, edit->mapToGlobal(at));
        QApplication::sendEvent(edit, &right);
        auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(popup != nullptr);
        QVERIFY(popup->actions().contains(lock));
        QVERIFY(popup->actions().contains(edit->toggleViewAction()));
        lock->trigger();
        popup->close();
        QVERIFY(app.toolbarsLocked());
        app.setToolbarsLocked(false);
    }

    // The lock is kept: a window opened with it set starts locked; the
    // settings dialog shows it, sets it and gives the default back.
    void theLockIsKeptAndSetInTheSettings()
    {
        LockGuard guard;
        QucsSettings.LockToolbars = true;
        {
            QucsApp app(false);
            MainGuard main(&app);
            QVERIFY(app.toolbarsLocked());
            QVERIFY(app.findChild<QAction*>(QStringLiteral("lockToolbars"))->isChecked());
            for (QToolBar* bar : app.toolbars()) QVERIFY(!bar->isMovable());

            QucsSettingsDialog dlg(&app);
            auto* check = dlg.findChild<QCheckBox*>(QStringLiteral("lockToolbarsCheck"));
            QVERIFY(check != nullptr);
            QVERIFY(check->isChecked());
            check->setChecked(false);
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QVERIFY(!app.toolbarsLocked());
            for (QToolBar* bar : app.toolbars()) QVERIFY(bar->isMovable());
            QVERIFY(!app.findChild<QAction*>(QStringLiteral("lockToolbars"))->isChecked());
            QCOMPARE(_settings::Get().item<bool>("LockToolbars"), false);

            check->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QVERIFY(app.toolbarsLocked());
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotDefaultValues"));
            QVERIFY(!check->isChecked());
            app.setToolbarsLocked(false);
        }
        // Saved with the other settings too.
        QucsSettings.LockToolbars = true;
        QVERIFY(saveApplSettings());
        QucsSettings.LockToolbars = false;
        QCOMPARE(_settings::Get().item<bool>("LockToolbars"), true);
    }
};

QTEST_MAIN(TestToolbarLock)
#include "test_toolbar_lock.moc"
