/*
 * The tuner is a dock of the main window, not a window of its own: shown
 * beside the simulation output while tuning is on, floatable, and closed
 * (by its button, the dock's, or Esc in it) it stops tuning. Esc elsewhere
 * in the window stays the window's (it stops the schematic's tool).
 *
 * (A modal dialog - the tuner's "update the values?" - would hang the
 * test: one that shows up fails it.)
 */
#include <QtTest>
#include <QDockWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "projectView.h"
#include "schematic.h"
#include "components/component.h"
#include "dialogs/tuner.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

void pump()
{
    for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
}
} // namespace

class TestTunerDock : public QObject
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

    void theTunerIsADockOfTheWindow()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QStringList asked;
        QTimer answer;
        connect(&answer, &QTimer::timeout, [&asked] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                asked << box->text();
                box->reject();
            }
        });
        answer.start(100);
        app.resize(1400, 900);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        const QString file = dir.filePath("rcl.sch");
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), file));
        QVERIFY(app.gotoPage(file, false, false));
        pump();
        auto* sch = qobject_cast<Schematic*>(app.DocumentTab->currentWidget());
        QVERIFY(sch != nullptr);

        app.tune->setChecked(true);
        pump();
        QVERIFY(app.TuningMode);
        QDockWidget* dock = app.tunerDockWidget();
        QVERIFY(dock != nullptr);
        QVERIFY(dock->isVisible());
        QCOMPARE(dock->widget(), static_cast<QWidget*>(app.tunerDia));
        QVERIFY(!app.tunerDia->isWindow());               // not a window of its own
        QCOMPARE(app.tunerDia->window(), static_cast<QWidget*>(&app));
        QCOMPARE(app.dockWidgetArea(dock), Qt::BottomDockWidgetArea);
        QVERIFY(dock->features() & QDockWidget::DockWidgetFloatable);
        QVERIFY(dock->features() & QDockWidget::DockWidgetMovable);

        // A property to tune: in the dock, in its scroll area.
        Component* r1 = nullptr;
        for (Component* c : sch->a_DocComps)
            if (c->Name == "R1") r1 = c;
        QVERIFY(r1 != nullptr);
        QVERIFY(isPropertyTunable(r1, r1->Props.first()));
        auto* element = new tunerElement(app.tunerDia, r1, r1->Props.first(), 0);
        app.tunerDia->addTunerElement(element);
        pump();
        auto* elements = app.tunerDia->findChild<QScrollArea*>("tunerElements");
        QVERIFY(elements != nullptr);
        QVERIFY(elements->widget()->isAncestorOf(element));
        QVERIFY(element->isVisibleTo(&app));

        // Esc elsewhere in the window is the window's (its action stops the
        // schematic's tool); the tuner stays. (In the active window: a
        // shortcut works in it alone.)
        app.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&app));
        QAction* escape = nullptr;
        for (QAction* a : app.actions())
            if (a->shortcut() == QKeySequence(Qt::Key_Escape)) escape = a;
        QVERIFY(escape != nullptr);
        QSignalSpy escaped(escape, &QAction::triggered);
        app.projectView()->setFocus();
        QTest::keyClick(app.projectView(), Qt::Key_Escape);
        pump();
        QCOMPARE(escaped.count(), 1);
        QVERIFY(app.tune->isChecked());
        QVERIFY(dock->isVisible());

        // Floated, it is a window; docked again, part of the main one.
        dock->setFloating(true);
        pump();
        QVERIFY(dock->isWindow());
        dock->setFloating(false);
        pump();
        QVERIFY(!dock->isWindow());

        // The dock closed: tuning stops, and the tuner goes - without asking
        // whether to keep values nobody changed (a value without a prefix,
        // R1's 30, was taken for changed).
        QPointer<TunerDialog> first = app.tunerDia;
        dock->close();
        pump();
        QVERIFY(!app.tune->isChecked());
        QVERIFY(!app.TuningMode);
        QVERIFY(!dock->isVisible());
        QTRY_VERIFY(first.isNull());

        // Tuning again: the tuner in the same dock; its Close button stops it.
        app.tune->setChecked(true);
        pump();
        QVERIFY(app.TuningMode);
        QCOMPARE(app.tunerDockWidget(), dock);
        QVERIFY(dock->isVisible());
        QCOMPARE(dock->widget(), static_cast<QWidget*>(app.tunerDia));
        QPushButton* close = nullptr;
        for (QPushButton* b : app.tunerDia->findChildren<QPushButton*>())
            if (b->text() == "Close") close = b;
        QVERIFY(close != nullptr);
        QPointer<TunerDialog> second = app.tunerDia;
        close->click();
        pump();
        QVERIFY(!app.tune->isChecked());
        QVERIFY(!app.TuningMode);
        QVERIFY(!dock->isVisible());
        QTRY_VERIFY(second.isNull());

        // Esc in the tuner closes it, before the window's Esc takes the key.
        app.tune->setChecked(true);
        pump();
        QVERIFY(dock->isVisible());
        QPointer<TunerDialog> third = app.tunerDia;
        QPushButton* focus = nullptr;
        for (QPushButton* b : third->findChildren<QPushButton*>())
            if (b->text() == "Close") focus = b;
        QVERIFY(focus != nullptr);
        focus->setFocusPolicy(Qt::StrongFocus);
        app.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&app));
        focus->setFocus();
        QVERIFY(third->isAncestorOf(QApplication::focusWidget()));
        escaped.clear();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        pump();
        QCOMPARE(escaped.count(), 0);
        QVERIFY(!app.tune->isChecked());
        QVERIFY(!dock->isVisible());
        QTRY_VERIFY(third.isNull());
        QCOMPARE(asked, QStringList());
        sch->setChanged(false);
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestTunerDock test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_tuner_dock.moc"
