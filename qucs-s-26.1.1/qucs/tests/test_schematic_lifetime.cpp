/*
 * Element lifetime across undo, redo, reload and destruction.
 *
 * Guards WS1.3 of ENHANCEMENT_PROPOSAL.md: a Schematic owns its elements,
 * frees them whenever the document is replaced, and announces the
 * replacement through signalDocumentRebuilt() so that nobody keeps a
 * pointer into the old document. Run under ASan to also catch leaks and
 * double frees.
 */
#include <QtTest>
#include <QSignalSpy>

#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "config.h"
#include "components/component.h"
#include "wire.h"
#include "node.h"
#include "diagrams/diagram.h"
#include "paintings/painting.h"
#include "extsimkernels/spicecompat.h"

class TestSchematicLifetime : public QObject
{
    Q_OBJECT

    QString example(const QString& relative) const
    {
        return QStringLiteral(QUCS_EXAMPLES_DIR) + QStringLiteral("/") + relative;
    }

    static void selectAll(Schematic& sch)
    {
        for (auto* c : sch.a_DocComps) c->isSelected = true;
        for (auto* w : sch.a_DocWires) w->isSelected = true;
        for (auto* d : sch.a_DocDiags) d->isSelected = true;
        for (auto* p : sch.a_DocPaints) p->isSelected = true;
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QDir(QStringLiteral(QUCS_EXAMPLES_DIR)).exists(),
                 "examples directory not found");
        // Register every component so any example loads regardless of
        // which simulator it targets.
        QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;
        QucsSettings.maxUndo = 20;   // as main() does
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void cleanup()
    {
        QucsSettings.maxUndo = 20;   // individual tests may lower it
    }

    void undoDepthOfZeroDoesNotBreakLoading()
    {
        // The settings dialog used to accept an undo depth of 0; loading a
        // document then popped the entry it had just pushed and read freed
        // memory. Depth 0 must simply mean "no undo available".
        QucsSettings.maxUndo = 0;
        Schematic sch(nullptr, example("ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        selectAll(sch);
        QVERIFY(sch.deleteElements());   // records the undo entry itself
        QVERIFY(!sch.undo());
        QVERIFY(!sch.redo());
        QCOMPARE(sch.a_DocComps.size(), std::size_t(0));
    }

    void loadsAnExample()
    {
        Schematic sch(nullptr, example("ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QSignalSpy rebuilt(&sch, &Schematic::signalDocumentRebuilt);
        QVERIFY(sch.load());
        QVERIFY(!sch.a_DocComps.empty());
        QVERIFY(!sch.a_DocWires.empty());
        QVERIFY(!sch.a_DocNodes.empty());
        QVERIFY(!sch.a_DocDiags.empty());
        QCOMPARE(rebuilt.count(), 1);   // a load replaces the (empty) document too
    }

    void undoRedoRestoreEveryElement()
    {
        Schematic sch(nullptr, example("ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        const auto comps = sch.a_DocComps.size();
        const auto wires = sch.a_DocWires.size();
        const auto diags = sch.a_DocDiags.size();
        const auto paints = sch.a_DocPaints.size();
        QVERIFY(comps > 0 && wires > 0);

        QSignalSpy rebuilt(&sch, &Schematic::signalDocumentRebuilt);

        // Delete everything, as Edit > Delete would.
        selectAll(sch);
        QVERIFY(sch.deleteElements());   // records the undo entry itself
        QCOMPARE(sch.a_DocComps.size(), std::size_t(0));
        QCOMPARE(sch.a_DocWires.size(), std::size_t(0));
        QCOMPARE(sch.a_DocNodes.size(), std::size_t(0));

        QVERIFY(sch.undo());
        QCOMPARE(rebuilt.count(), 1);
        QCOMPARE(sch.a_DocComps.size(), comps);
        QCOMPARE(sch.a_DocWires.size(), wires);
        QCOMPARE(sch.a_DocDiags.size(), diags);
        QCOMPARE(sch.a_DocPaints.size(), paints);
        QVERIFY(!sch.a_DocNodes.empty());

        QVERIFY(sch.redo());
        QCOMPARE(rebuilt.count(), 2);
        QCOMPARE(sch.a_DocComps.size(), std::size_t(0));

        QVERIFY(sch.undo());
        QCOMPARE(rebuilt.count(), 3);
        QCOMPARE(sch.a_DocComps.size(), comps);
    }

    void undoInvalidatesOldPointers()
    {
        Schematic sch(nullptr, example("ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        Component* const before = sch.a_DocComps.front();
        const QString name = before->Name;

        selectAll(sch);
        QVERIFY(sch.deleteElements());   // records the undo entry itself
        QVERIFY(sch.undo());

        // The document is rebuilt from the snapshot: same content, new
        // objects. Anyone holding `before` must have re-resolved by name.
        Component* const after = sch.getComponentByName(name);
        QVERIFY(after != nullptr);
        QCOMPARE(after->Name, name);
        QVERIFY(std::find(sch.a_DocComps.begin(), sch.a_DocComps.end(), before)
                == sch.a_DocComps.end());
    }

    void repeatedUndoRedoIsStable()
    {
        Schematic sch(nullptr, example("ngspice/General Electronics/chargepump.sch"));
        QVERIFY(sch.load());
        const auto comps = sch.a_DocComps.size();
        selectAll(sch);
        QVERIFY(sch.deleteElements());   // records the undo entry itself
        // Every cycle frees the previous document; ASan reports a leak or
        // a double free if ownership is wrong.
        for (int i = 0; i < 25; ++i) {
            QVERIFY(sch.undo());
            QCOMPARE(sch.a_DocComps.size(), comps);
            QVERIFY(sch.redo());
            QCOMPARE(sch.a_DocComps.size(), std::size_t(0));
        }
    }

    void reloadReplacesTheDocument()
    {
        Schematic sch(nullptr, example("ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        Component* const before = sch.a_DocComps.front();
        const auto comps = sch.a_DocComps.size();
        QSignalSpy rebuilt(&sch, &Schematic::signalDocumentRebuilt);

        QVERIFY(sch.load());
        QCOMPARE(rebuilt.count(), 1);
        QCOMPARE(sch.a_DocComps.size(), comps);
        QVERIFY(std::find(sch.a_DocComps.begin(), sch.a_DocComps.end(), before)
                == sch.a_DocComps.end());
    }

    void deleteAllElementsEmptiesTheDocument()
    {
        Schematic sch(nullptr, example("ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        sch.deleteAllElements();
        QVERIFY(sch.a_DocComps.empty());
        QVERIFY(sch.a_DocWires.empty());
        QVERIFY(sch.a_DocNodes.empty());
        QVERIFY(sch.a_DocDiags.empty());
        QVERIFY(sch.a_DocPaints.empty());
        // Deleting twice must be harmless.
        sch.deleteAllElements();
    }

    void destructorFreesTheDocument()
    {
        // Nothing to assert directly: a leak or double free shows up under
        // the sanitizers, and the loop would crash on a dangling node.
        for (int i = 0; i < 5; ++i) {
            auto* sch = new Schematic(nullptr, example("ngspice/RF/Miscellaneous/stab.sch"));
            QVERIFY(sch->load());
            QVERIFY(!sch->a_DocComps.empty());
            delete sch;
        }
    }
};

QTEST_MAIN(TestSchematicLifetime)
#include "test_schematic_lifetime.moc"
