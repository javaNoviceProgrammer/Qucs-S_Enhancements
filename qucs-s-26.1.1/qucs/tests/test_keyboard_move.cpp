/*
 * Moving the selection with the cursor keys (#1525): the document is
 * modified and one undo step covers the whole sequence of key presses;
 * while that step is the latest, Escape takes the move back. Upstream
 * neither marked the document changed nor recorded the move.
 */
#include <QtTest>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QList<QPoint> positions(Schematic* doc)
{
    QList<QPoint> p;
    for (Component* c : doc->a_DocComps) p << QPoint(c->cx, c->cy);
    return p;
}

QList<QPoint> shifted(QList<QPoint> p, int dx, int dy)
{
    for (QPoint& q : p) q += QPoint(dx, dy);
    return p;
}
} // namespace

class TestKeyboardMove : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString sch;

    static void selectAll(Schematic* doc)
    {
        for (Component* c : doc->a_DocComps) c->isSelected = true;
    }

    static int undoDepth(Schematic* doc)
    {
        int n = 0;
        while (doc->undo()) ++n;
        while (doc->redo()) {}
        return n;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        // QucsApp lists the simulators it can find and puts up a modal
        // error box when there is none: name one that exists.
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        // Same name as the example: a renamed copy would prompt (modally)
        // to rename its dataset and display files on open.
        sch = dir.filePath("RCL_resonance.sch");
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), sch));
    }

    void cursorKeysMoveModifyAndUndoAsOneStep()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        QVERIFY(!doc->getDocChanged());
        const QList<QPoint> before = positions(doc);
        QVERIFY(!before.isEmpty());
        const int gx = doc->getGridX(), gy = doc->getGridY();

        selectAll(doc);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotCursorLeft", Q_ARG(bool, true)));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotCursorLeft", Q_ARG(bool, true)));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotCursorUp", Q_ARG(bool, false)));   // down
        QCOMPARE(positions(doc), shifted(before, -2 * gx, gy));
        QVERIFY(doc->getDocChanged());        // upstream left the document "unchanged"

        // One undo step for the three key presses, and it works.
        QVERIFY(doc->undo());
        QCOMPARE(positions(doc), before);
        QVERIFY(!doc->undo());                 // nothing older than the loaded state
        QVERIFY(!doc->getDocChanged());
        QVERIFY(doc->redo());
        QCOMPARE(positions(doc), shifted(before, -2 * gx, gy));
        doc->setDocChanged(false);
    }

    void escapeTakesTheMoveBack()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        const QList<QPoint> before = positions(doc);
        const int gx = doc->getGridX();

        selectAll(doc);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotCursorLeft", Q_ARG(bool, false)));   // right
        QVERIFY(QMetaObject::invokeMethod(&app, "slotCursorLeft", Q_ARG(bool, false)));
        QCOMPARE(positions(doc), shifted(before, 2 * gx, 0));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEscape"));
        QCOMPARE(positions(doc), before);
        QVERIFY(!doc->getDocChanged());        // back at the loaded state

        // Escape with no move to cancel does what it always did (select
        // mode), and does not undo anything else.
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEscape"));
        QCOMPARE(positions(doc), before);
        QVERIFY(app.select->isChecked());

        // A new sequence after a mouse press (which ends the sequence with
        // endKeyboardMove()) is its own step: Escape takes back only the
        // second one.
        selectAll(doc);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotCursorUp", Q_ARG(bool, true)));
        doc->endKeyboardMove();
        selectAll(doc);
        QVERIFY(QMetaObject::invokeMethod(&app, "slotCursorUp", Q_ARG(bool, true)));
        const int gy = doc->getGridY();
        QCOMPARE(positions(doc), shifted(before, 0, -2 * gy));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEscape"));
        QCOMPARE(positions(doc), shifted(before, 0, -gy));
        QVERIFY(doc->getDocChanged());
        QVERIFY(QMetaObject::invokeMethod(&app, "slotEscape"));   // the first sequence is closed: stays
        QCOMPARE(positions(doc), shifted(before, 0, -gy));
        doc->setDocChanged(false);
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestKeyboardMove test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_keyboard_move.moc"
