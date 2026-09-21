/*
 * Net highlighting: the wires and nodes electrically connected to a
 * selected wire - through nodes, through labels of the same name and
 * through ground symbols - are painted with a glow under them.
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
#include "wire.h"
#include "node.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

Wire* wireAt(Schematic* doc, int x1, int y1, int x2, int y2)
{
    for (Wire* w : doc->a_DocWires)
        if (w->x1 == x1 && w->y1 == y1 && w->x2 == x2 && w->y2 == y2) return w;
    return nullptr;
}

// How many pixels show the glow's orange over the paper (about
// 255,198,119 on the default paper; a wire's blue or red text is not it).
int glowPixels(const QImage& img)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.red() > 230 && c.green() > 170 && c.green() < 215 && c.blue() > 90 && c.blue() < 140) ++n;
        }
    return n;
}
} // namespace

class TestNetHighlight : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString sch;

    static void write(const QString& path, const QByteArray& bytes)
    {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.BGColor = QColor(255, 250, 225);
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        // Three nets: A is two wires meeting at a node plus a wire joined
        // by the label "a"; G is two wires that only share a ground; B is
        // a wire on its own.
        sch = dir.filePath("nets.sch");
        write(sch,
            "<Qucs Schematic " PACKAGE_VERSION ">\n"
            "<Components>\n"
            "  <GND * 1 100 300 0 0 0 0>\n"
            "  <GND * 1 300 300 0 0 0 0>\n"
            "</Components>\n"
            "<Wires>\n"
            "  <100 100 200 100 \"\" 0 0 0 \"\">\n"          // A1
            "  <200 100 200 150 \"a\" 220 120 20 \"\">\n"    // A2, meets A1 at (200,100), label a
            "  <400 100 500 100 \"a\" 450 80 30 \"\">\n"     // A3, joined by the label
            "  <100 250 100 300 \"\" 0 0 0 \"\">\n"          // G1, to the first ground
            "  <300 250 300 300 \"\" 0 0 0 \"\">\n"          // G2, to the second ground
            "  <600 200 700 200 \"\" 0 0 0 \"\">\n"          // B, alone
            "</Wires>\n"
            "<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
    }

    void theNetFollowsNodesLabelsAndGround()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch, false, false));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        Wire* a1 = wireAt(doc, 100, 100, 200, 100);
        Wire* a2 = wireAt(doc, 200, 100, 200, 150);
        Wire* a3 = wireAt(doc, 400, 100, 500, 100);
        Wire* g1 = wireAt(doc, 100, 250, 100, 300);
        Wire* g2 = wireAt(doc, 300, 250, 300, 300);
        Wire* b  = wireAt(doc, 600, 200, 700, 200);
        QVERIFY(a1 && a2 && a3 && g1 && g2 && b);

        QVERIFY(doc->selectedNet().empty());   // nothing selected: no net

        const Schematic::Net netA = doc->netOf(a1);
        QVERIFY(netA.wires.count(a1));
        QVERIFY(netA.wires.count(a2));         // through the node at (200,100)
        QVERIFY(netA.wires.count(a3));         // through the label "a"
        QVERIFY(!netA.wires.count(g1) && !netA.wires.count(g2) && !netA.wires.count(b));
        QCOMPARE(int(netA.wires.size()), 3);
        QVERIFY(netA.nodes.count(a1->Port1) && netA.nodes.count(a1->Port2));
        QVERIFY(netA.nodes.count(a3->Port1) && netA.nodes.count(a3->Port2));
        QCOMPARE(doc->netOf(a3).wires, netA.wires);   // the same net from the other end

        const Schematic::Net netG = doc->netOf(g1);
        QVERIFY(netG.wires.count(g1) && netG.wires.count(g2));   // grounds are one net
        QCOMPARE(int(netG.wires.size()), 2);

        const Schematic::Net netB = doc->netOf(b);
        QCOMPARE(int(netB.wires.size()), 1);
        QCOMPARE(int(netB.nodes.size()), 2);

        // Selection drives it; two selected wires of different nets add up.
        a1->isSelected = true;
        QCOMPARE(doc->selectedNet().wires, netA.wires);
        b->isSelected = true;
        QCOMPARE(int(doc->selectedNet().wires.size()), 4);
        a1->isSelected = b->isSelected = false;
        QVERIFY(doc->selectedNet().empty());
    }

    void theGlowIsPaintedOnlyWhileAWireIsSelected()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 700);
        app.show();
        QVERIFY(app.gotoPage(sch, false, false));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        doc->showAll();
        QCoreApplication::processEvents();
        const int plain = glowPixels(doc->viewport()->grab().toImage());
        QCOMPARE(plain, 0);

        wireAt(doc, 100, 100, 200, 100)->isSelected = true;
        doc->viewport()->update();
        QCoreApplication::processEvents();
        const int lit = glowPixels(doc->viewport()->grab().toImage());
        QVERIFY2(lit > 200, qPrintable(QString::number(lit)));   // wires and nodes of net A
        // For a look: QUCS_TEST_GRAB=<dir> saves a picture of the lit net.
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (!grabDir.isEmpty()) doc->viewport()->grab().save(grabDir + "/net-highlight.png");

        wireAt(doc, 100, 100, 200, 100)->isSelected = false;
        doc->viewport()->update();
        QCoreApplication::processEvents();
        QCOMPARE(glowPixels(doc->viewport()->grab().toImage()), 0);

        // Not in an exported picture: the export paints without the selection.
        wireAt(doc, 100, 100, 200, 100)->isSelected = true;
        const QString png = dir.filePath("nets.png");
        QImage out(600, 400, QImage::Format_ARGB32);
        out.fill(Qt::white);
        QPainter p(&out);
        doc->paintSchToViewpainter(&p, true);
        p.end();
        QCOMPARE(glowPixels(out), 0);
        wireAt(doc, 100, 100, 200, 100)->isSelected = false;
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestNetHighlight test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_net_highlight.moc"
