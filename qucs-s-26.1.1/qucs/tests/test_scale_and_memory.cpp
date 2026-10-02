/*
 * What a stress test of large schematics found (2026-10-01), one test each:
 *
 * - components never freed what they own - their symbol's lines, arcs and
 *   texts, their ports, their properties - nor did a symbol made again
 *   (each edit of a MOSFET), a component copied into another, or a
 *   diagram's grid drawn again: gigabytes after closing every document;
 * - more than 1000 vectors in one ngspice command (a write of every net of
 *   1,250 stages) wrote nothing - "write: too many args" - and the run read
 *   as a success with an empty dataset;
 * - loading a component split its whole line once per field;
 * - each undo step kept the whole schematic as text (15 MB at 10,000
 *   parts), and a dataset was read whole into memory to be parsed.
 *
 * The lists' freeing is counted exactly (their elements' destructors are
 * virtual); the heap's size is checked where the platform tells it.
 */
#include <QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <memory>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define QUCS_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define QUCS_TEST_ASAN 1
#endif
#if defined(QUCS_TEST_ASAN) && __has_include(<sanitizer/allocator_interface.h>)
#include <sanitizer/allocator_interface.h>
#elif defined(QUCS_TEST_ASAN)
// GCC installs no allocator_interface.h; its libasan has the function.
#include <cstddef>
extern "C" size_t __sanitizer_get_current_allocated_bytes();
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "config.h"
#include "dataset.h"
#include "simulatorlog.h"
#include "components/component.h"
#include "components/capacitor.h"
#include "components/property.h"
#include "diagrams/curvediagram.h"
#include "diagrams/graph.h"
#include "diagrams/polardiagram.h"
#include "diagrams/psdiagram.h"
#include "diagrams/rect3ddiagram.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/smithdiagram.h"
#include "diagrams/tabdiagram.h"
#include "diagrams/timingdiagram.h"
#include "diagrams/truthdiagram.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"

namespace {

int g_freed = 0;

struct CountedLine : qucs::Line {
    CountedLine() : qucs::Line(0, 0, 1, 1, QPen()) {}
    ~CountedLine() override { ++g_freed; }
};
struct CountedArc : qucs::Arc {
    CountedArc() : qucs::Arc(0, 0, 1, 1, 0, 16 * 360, QPen()) {}
    ~CountedArc() override { ++g_freed; }
};
struct CountedText : Text {
    CountedText() : Text(0, 0, QStringLiteral("t")) {}
    ~CountedText() override { ++g_freed; }
};

struct Taker : Component {
    void take(Component* other) { copyComponent(other); }
};

// Bytes in use on the heap; -1 where the platform does not tell.
qint64 heapInUse()
{
#if defined(QUCS_TEST_ASAN)
    return qint64(__sanitizer_get_current_allocated_bytes());
#elif defined(__APPLE__)
    malloc_statistics_t s{};
    malloc_zone_statistics(nullptr, &s);
    return qint64(s.size_in_use);
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    return qint64(mallinfo2().uordblks);
#else
    return -1;
#endif
}

Component* fromLine(QString line)
{
    return getComponentFromName(line, nullptr);
}

QString header()
{
    return QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <View=0,0,2000,2000,1,0,0>\n"
                          "  <Grid=10,10,1>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n");
}

// N resistors in rows of 100, one chain from a source, each net between
// two of them labelled n1, n2, ...; \a more: more components.
QString labelledChain(int n, const QString& more = QString())
{
    QString comps = QStringLiteral("  <Vdc V1 1 -60 60 18 -26 0 1 \"5 V\" 1>\n  <GND * 1 -60 90 0 0 0 0>\n") + more;
    QString wires = QStringLiteral("  <-60 30 70 30 \"in\" 0 0 0 \"\">\n");
    for (int k = 0; k < n; ++k) {
        const int row = k / 100, col = k % 100;
        const int x = 100 + 100 * col, y = 30 + 100 * row;
        comps += QStringLiteral("  <R R%1 1 %2 %3 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
                     .arg(k + 1).arg(x).arg(y);
        const QString label = QStringLiteral("n%1").arg(k + 1);
        if (col < 99 && k < n - 1) {
            wires += QStringLiteral("  <%1 %2 %3 %2 \"%4\" %1 %2 0 \"\">\n").arg(x + 30).arg(y).arg(x + 70).arg(label);
        } else if (k < n - 1) {
            wires += QStringLiteral("  <%1 %2 %1 %3 \"%4\" %1 %2 0 \"\">\n").arg(x + 30).arg(y).arg(y + 50).arg(label);
            wires += QStringLiteral("  <70 %1 %2 %1 \"\" 0 0 0 \"\">\n").arg(y + 50).arg(x + 30);
            wires += QStringLiteral("  <70 %1 70 %2 \"\" 0 0 0 \"\">\n").arg(y + 50).arg(y + 100);
        }
    }
    return header() + comps + QStringLiteral("</Components>\n<Wires>\n") + wires
           + QStringLiteral("</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
}

class KernelProbe : public Ngspice
{
public:
    using Ngspice::Ngspice;
    void setOutputs(const QStringList& files) { a_output_files = files; }
};

// An AC plot of ngspice's, binary: v(out) at one frequency.
QByteArray acPlot()
{
    QByteArray raw = "Title: t\nDate: d\nPlotname: AC Analysis\nFlags: complex\nNo. Variables: 2\nNo. Points: 1\n"
                     "Variables:\n\t0\tfrequency\tfrequency\n\t1\tv(out)\tvoltage\nBinary:\n";
    QDataStream s(&raw, QIODevice::Append);
    s.setByteOrder(QDataStream::LittleEndian);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);
    for (double v : {1.0, 0.0, 0.5, 0.25}) s << v;
    return raw;
}

} // namespace

class TestScaleAndMemory : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    bool write(const QString& name, const QByteArray& bytes)
    {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        return f.write(bytes) == bytes.size();
    }
    QByteArray read(const QString& name) const
    {
        QFile f(dir.filePath(name));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsSettings.QucsWorkDir.setPath(dir.path());
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // A component deleted deletes its symbol, ports and properties; a
    // component copied into another takes them over (the other keeps
    // none, and its own are freed); a symbol made again frees the old.
    void componentsFreeWhatTheyOwn()
    {
        g_freed = 0;
        Component* r = fromLine(QStringLiteral("<R R1 1 0 0 -26 15 0 0 \"1k\" 1>"));
        QVERIFY(r != nullptr);
        r->Lines.append(new CountedLine);
        r->Arcs.append(new CountedArc);
        r->Texts.append(new CountedText);
        delete r;
        QCOMPARE(g_freed, 3);

        // (As a library component takes over the one its model makes.)
        g_freed = 0;
        auto* a = new Taker;
        a->Lines.append(new CountedLine);
        a->Props.append(new Property("x", "1", true));
        Component* b = fromLine(QStringLiteral("<R R2 1 0 0 -26 15 0 0 \"2k\" 1>"));
        b->Lines.append(new CountedLine);
        b->Texts.append(new CountedText);
        const auto bLines = b->Lines.size();
        a->take(b);
        QCOMPARE(g_freed, 1);   // a's own
        QCOMPARE(a->Lines.size(), bLines);
        QVERIFY(b->Lines.isEmpty() && b->Props.isEmpty() && b->Ports.isEmpty() && b->Texts.isEmpty());
        QCOMPARE(a->Props.first()->Value, QStringLiteral("2k"));
        delete b;   // (nothing of a's freed twice: ASan says so if it is)
        QCOMPARE(g_freed, 1);
        delete a;
        QCOMPARE(g_freed, 3);

        g_freed = 0;
        Capacitor c;
        c.Lines.append(new CountedLine);
        c.Texts.append(new CountedText);
        const auto lines = c.Lines.size() - 1;
        c.recreate();
        QCOMPARE(g_freed, 2);
        QCOMPARE(c.Lines.size(), lines);
    }

    // Every diagram's grid, drawn again, frees the one before.
    void diagramsFreeTheirOldGrid()
    {
        std::vector<std::unique_ptr<Diagram>> diagrams;
        diagrams.emplace_back(new RectDiagram);
        diagrams.emplace_back(new Rect3DDiagram);
        diagrams.emplace_back(new CurveDiagram);
        diagrams.emplace_back(new PSDiagram);
        diagrams.emplace_back(new PolarDiagram);
        diagrams.emplace_back(new SmithDiagram);
        diagrams.emplace_back(new TabDiagram);
        diagrams.emplace_back(new TimingDiagram);
        diagrams.emplace_back(new TruthDiagram);
        for (auto& d : diagrams) {
            g_freed = 0;
            d->Lines.append(new CountedLine);
            d->Texts.append(new CountedText);
            d->Arcs.append(new CountedArc);
            d->calcDiagram();
            QVERIFY2(g_freed == 3, qPrintable(d->Name + ": " + QString::number(g_freed)));
        }
    }

    // Thousands of components made and deleted: the heap comes back to
    // where it was (it grew by a kilobyte and more for each).
    void deletedComponentsGiveTheirMemoryBack()
    {
        if (heapInUse() < 0) QSKIP("the heap's size is not told here");
        const auto cycle = [] {
            std::vector<Component*> made;
            for (int k = 0; k < 2000; ++k) {
                made.push_back(fromLine(QStringLiteral("<R R%1 1 0 0 -26 15 0 0 \"1k\" 1>").arg(k)));
                made.push_back(fromLine(QStringLiteral("<C C%1 1 0 0 -26 15 0 0 \"1p\" 1>").arg(k)));
            }
            for (Component* c : made) {
                QVERIFY(c != nullptr);
                if (auto* m = dynamic_cast<MultiViewComponent*>(c)) m->recreate();   // (its symbol made again)
            }
            qDeleteAll(made);
        };
        cycle();   // (whatever is made once, the first time)
        const qint64 before = heapInUse();
        for (int i = 0; i < 3; ++i) cycle();
        const qint64 grown = heapInUse() - before;
        QVERIFY2(grown < 1024 * 1024, qPrintable(QStringLiteral("%1 bytes more after 12,000 components").arg(grown)));
    }

    // Each field of a component's line, as section() read it: the head
    // before the first quote, the values between quotes (with their \n and
    // '' for " ), fewer values than the type has (the rest as they were).
    void aComponentLineIsReadFieldByField()
    {
        std::unique_ptr<Component> r(fromLine(QStringLiteral(
            "<R R1 5 -120 340 -26 15 1 3 \"1 k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>")));
        QVERIFY(r != nullptr);
        QCOMPARE(r->Name, QStringLiteral("R1"));
        QCOMPARE(r->isActive, COMP_IS_ACTIVE);
        QVERIFY(!r->showName);   // 4 in the second field
        QCOMPARE(r->cx, -120);
        QCOMPARE(r->cy, 340);
        QVERIFY(r->mirroredX);
        QCOMPARE(r->rotated, 3);
        QCOMPARE(r->Props.at(0)->Value, QStringLiteral("1 k"));
        QVERIFY(r->Props.at(0)->display);
        QCOMPARE(r->Props.at(5)->Value, QStringLiteral("US"));
        QVERIFY(!r->Props.at(5)->display);

        std::unique_ptr<Component> fewer(fromLine(QStringLiteral("<R R2 2 0 0 -26 15 0 0 \"2k\" 0>")));
        QVERIFY(fewer != nullptr);
        QCOMPARE(fewer->isActive, COMP_IS_SHORTEN);
        QCOMPARE(fewer->Props.at(0)->Value, QStringLiteral("2k"));
        QVERIFY(!fewer->Props.at(0)->display);
        QCOMPARE(fewer->Props.at(1)->Value, QStringLiteral("26.85"));   // as it was

        std::unique_ptr<Component> ground(fromLine(QStringLiteral("<GND * 1 -60 90 0 0 0 0>")));
        QVERIFY(ground != nullptr);
        QCOMPARE(ground->Name, QString());
        QCOMPARE(ground->cx, -60);
        QCOMPARE(ground->cy, 90);

        std::unique_ptr<Component> eqn(fromLine(QStringLiteral(
            "<Eqn Eqn1 1 300 400 -30 15 0 0 \"y=1 + 2\" 1 \"z=''a b''\\nnext\" 1 \"yes\" 0>")));
        QVERIFY(eqn != nullptr);
        QCOMPARE(eqn->Props.size(), 3);
        QCOMPARE(eqn->Props.at(0)->Name, QStringLiteral("y"));
        QCOMPARE(eqn->Props.at(0)->Value, QStringLiteral("1 + 2"));
        QCOMPARE(eqn->Props.at(1)->Name, QStringLiteral("z"));
        QCOMPARE(eqn->Props.at(1)->Value, QStringLiteral("\"a b\"\nnext"));
        QCOMPARE(eqn->Props.at(2)->Value, QStringLiteral("yes"));

        std::unique_ptr<Component> tr(fromLine(QStringLiteral(
            "<.TR TR1 1 200 2450 0 50 0 0 \"lin\" 1 \"0\" 1 \"1 us\" 1 \"2001\" 0>")));
        QVERIFY(tr != nullptr);
        QCOMPARE(tr->cx, 200);
        QCOMPARE(tr->Props.at(2)->Value, QStringLiteral("1 us"));
        QCOMPARE(tr->Props.at(3)->Value, QStringLiteral("2001"));

        QVERIFY(fromLine(QStringLiteral("<R R3 x 0 0 -26 15 0 0 \"1k\" 1>")) == nullptr);   // not a number
        QVERIFY(fromLine(QStringLiteral("<R R3 1 0>")) == nullptr);                         // cut short
    }

    // ngspice takes 1000 words of a command: vectors in chunks it takes,
    // each whole, in order.
    void vectorsComeInChunksNgspiceTakes()
    {
        QStringList many;
        for (int k = 0; k < 2001; ++k) many << QStringLiteral("v(n%1)").arg(k);
        const QStringList chunks = spicecompat::vectorChunks(many.join(QStringLiteral("  ")) + QStringLiteral(" \n"));
        QCOMPARE(chunks.size(), 3);
        QCOMPARE(chunks.at(0).split(' ').size(), spicecompat::ngspiceMostVectors);
        QCOMPARE(chunks.join(' '), many.join(' '));
        QVERIFY(spicecompat::tooManyVectors(many.join(' ')));
        QVERIFY(!spicecompat::tooManyVectors(many.mid(0, spicecompat::ngspiceMostVectors).join(' ')));
        QVERIFY(spicecompat::vectorChunks(QStringLiteral("  ")).isEmpty());
        QCOMPARE(spicecompat::saveLines(QStringLiteral("v(a) v(b)")), QStringLiteral("save v(a) v(b)\n"));
        QCOMPARE(spicecompat::saveLines(many.join(' ')).count(QStringLiteral("save ")), 3);
    }

    // A netlist of more nets than a command takes: the nets saved before
    // the transient in commands ngspice takes, its plot written whole, the
    // saves cleared after it; the DC point printed in parts into one file.
    // With equations, nothing saved: they may read what a save leaves out.
    void aNetlistOfManyNetsWritesThemAll()
    {
        const QString analyses = QStringLiteral(
            "  <.TR TR1 1 200 -200 0 50 0 0 \"lin\" 1 \"0\" 1 \"1 us\" 1 \"11\" 0>\n"
            "  <.DC DC1 1 400 -200 0 33 0 0>\n");
        QVERIFY(write("many.sch", labelledChain(1100, analyses).toUtf8()));
        Schematic sch(nullptr, dir.filePath("many.sch"));
        QVERIFY(sch.loadDocument());
        const QString netlist = dir.filePath("many.cir");
        {
            Ngspice kernel(&sch);
            kernel.SaveNetlist(netlist, false);
        }
        const QString text = QString::fromUtf8(read("many.cir"));
        const QStringList lines = text.split('\n');
        for (const QString& line : lines) QVERIFY2(line.split(' ').size() < 1000, qPrintable(line.left(100)));
        const QStringList saves = lines.filter(QRegularExpression(QStringLiteral("^save ")));
        QVERIFY2(saves.size() >= 2, qPrintable(text.right(2000)));
        QVERIFY(saves.join(' ').contains(QStringLiteral(" v(n1099) ")));
        QVERIFY(saves.join(' ').contains(QStringLiteral("v(in)")));
        const int save = text.indexOf(QStringLiteral("\nsave "));
        const int tran = text.indexOf(QStringLiteral("\ntran "));
        const int written = text.indexOf(QStringLiteral("\nwrite spice4qucs.tr1.plot\n"));
        const int cleared = text.indexOf(QStringLiteral("destroy all\ndelete all\n"));
        QVERIFY2(save > 0 && tran > save && written > tran && cleared > written, qPrintable(text.right(3000)));
        const QStringList prints = lines.filter(QRegularExpression(QStringLiteral("^print ")));
        QVERIFY2(prints.size() >= 2, qPrintable(text.right(2000)));
        QVERIFY(prints.first().endsWith(QStringLiteral(" > spice4qucs.dc1.ngspice.dc.print")));
        for (const QString& p : prints.mid(1)) QVERIFY(p.endsWith(QStringLiteral(" >> spice4qucs.dc1.ngspice.dc.print")));
        // The DC point's saves are not the transient's: cleared before it.
        QVERIFY(text.indexOf(QStringLiteral("\nop")) < save || text.indexOf(QStringLiteral("\nop")) > cleared);

        const QString withEquation = labelledChain(1100, analyses + QStringLiteral(
            "  <Eqn Eqn1 1 600 -300 -30 15 0 0 \"twice=v(n5)*2\" 1 \"yes\" 0>\n"));
        QVERIFY(write("many_eqn.sch", withEquation.toUtf8()));
        Schematic eq(nullptr, dir.filePath("many_eqn.sch"));
        QVERIFY(eq.loadDocument());
        {
            Ngspice kernel(&eq);
            kernel.SaveNetlist(dir.filePath("many_eqn.cir"), false);
        }
        const QString read2 = QString::fromUtf8(read("many_eqn.cir"));
        QVERIFY2(!read2.contains(QStringLiteral("\nsave ")), qPrintable(read2.right(2000)));
        QVERIFY(read2.contains(QStringLiteral("\nwrite spice4qucs.tr1.plot\n")));
        QVERIFY(!read2.contains(QStringLiteral("delete all")));

        // A few nets: one write naming them, as before.
        QVERIFY(write("few.sch", labelledChain(20, analyses).toUtf8()));
        Schematic few(nullptr, dir.filePath("few.sch"));
        QVERIFY(few.loadDocument());
        {
            Ngspice kernel(&few);
            kernel.SaveNetlist(dir.filePath("few.cir"), false);
        }
        const QString read3 = QString::fromUtf8(read("few.cir"));
        QVERIFY(!read3.contains(QStringLiteral("\nsave ")));
        QVERIFY(read3.contains(QStringLiteral("\nwrite spice4qucs.tr1.plot v(")));
        QCOMPARE(read3.count(QStringLiteral("\nprint ")), 1);

        // No net named: no print of nothing ("print: too few args."), the
        // devices' values (show all) the DC point's results.
        QVERIFY(write("unnamed.sch", (header() + QStringLiteral(
            "  <Vdc V1 1 100 200 18 -26 0 1 \"1 V\" 1>\n"
            "  <R R1 1 200 200 15 -26 0 1 \"50 Ohm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <GND * 1 100 300 0 0 0 0>\n  <.DC DC1 1 300 100 0 36 0 0>\n</Components>\n<Wires>\n"
            "  <100 170 200 170 \"\" 0 0 0 \"\">\n  <100 230 100 300 \"\" 0 0 0 \"\">\n  <200 230 200 300 \"\" 0 0 0 \"\">\n"
            "  <100 300 200 300 \"\" 0 0 0 \"\">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")).toUtf8()));
        Schematic unnamed(nullptr, dir.filePath("unnamed.sch"));
        QVERIFY(unnamed.loadDocument());
        {
            Ngspice kernel(&unnamed);
            kernel.SaveNetlist(dir.filePath("unnamed.cir"), false);
        }
        const QString read4 = QString::fromUtf8(read("unnamed.cir"));
        QVERIFY2(!read4.contains(QStringLiteral("\nprint")) && read4.contains(QStringLiteral("\nop\nshow all > ")), qPrintable(read4));
    }

    // No results where results were expected: an error, and the dataset
    // from before stays as it was (an empty one read as a run that worked).
    // A read-only dataset is not written over; a good run replaces it.
    void anEmptyOutputIsAnError()
    {
        Schematic sch(nullptr, QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        const QString work = dir.filePath("convert");
        QVERIFY(QDir().mkpath(work));
        const QByteArray old = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 1>\n  0\n</indep>\n";
        QVERIFY(write("convert/out.dat", old));

        KernelProbe kernel(&sch);
        kernel.setWorkdir(work);
        kernel.setOutputs({QStringLiteral("spice4qucs.tr1.plot")});
        QString error = kernel.convertToQucsData(work + "/out.dat");   // no output at all
        QVERIFY2(error.contains(QStringLiteral("wrote no results")), qPrintable(error));
        QVERIFY(error.contains(QStringLiteral("spice4qucs.tr1.plot")));
        QCOMPARE(read("convert/out.dat"), old);
        QVERIFY(write("convert/spice4qucs.tr1.plot", ""));   // an empty one
        error = kernel.convertToQucsData(work + "/out.dat");
        QVERIFY2(error.contains(QStringLiteral("wrote no results")), qPrintable(error));
        QCOMPARE(read("convert/out.dat"), old);

        QVERIFY(write("convert/spice4qucs.ac.plot", acPlot()));
        kernel.setOutputs({QStringLiteral("spice4qucs.ac.plot")});
        QFile::setPermissions(work + "/out.dat", QFileDevice::ReadOwner);
        error = kernel.convertToQucsData(work + "/out.dat");
        QVERIFY2(error.contains(QStringLiteral("read-only")), qPrintable(error));
        QCOMPARE(read("convert/out.dat"), old);
        QFile::setPermissions(work + "/out.dat", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        error = kernel.convertToQucsData(work + "/out.dat");
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(read("convert/out.dat").contains("v(out)"));
        // (No file of QSaveFile's left beside it.)
        for (const QString& f : QDir(work).entryList(QDir::Files))
            QVERIFY2(f == "out.dat" || f.startsWith("spice4qucs."), qPrintable(f));

        // A folder that takes no new file, the dataset in it writable: no
        // results leave the dataset as it was (it was cut to its first
        // line, written in place), and results are written over it.
        const QString locked = dir.filePath("locked");
        QVERIFY(QDir().mkpath(locked));
        QVERIFY(write("locked/out.dat", old));
        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto unlock = qScopeGuard([&locked] {
            QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        });
        QVERIFY(write("convert/spice4qucs.ac.plot", ""));
        error = kernel.convertToQucsData(locked + "/out.dat");
        QVERIFY2(error.contains(QStringLiteral("wrote no results")), qPrintable(error));
        QCOMPARE(read("locked/out.dat"), old);
        QVERIFY(write("convert/spice4qucs.ac.plot", acPlot()));
        error = kernel.convertToQucsData(locked + "/out.dat");
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(read("locked/out.dat").contains("v(out)"));
        QVERIFY(read("locked/out.dat").startsWith("<Qucs Dataset "));
        QCOMPARE(QDir(locked).entryList(QDir::Files), QStringList{"out.dat"});
        // (And no dataset there yet: none can be made, as before.)
        error = kernel.convertToQucsData(locked + "/new.dat");
        QVERIFY2(error.contains(QStringLiteral("Check write permission")), qPrintable(error));
    }

    // "write: too many args." is an error of the run, in the log and in
    // the problems told.
    void tooManyArgsIsAnError()
    {
        const QString log = QStringLiteral("Circuit: stress\nDoing analysis at TEMP = 27.000000\n"
                                           "No. of Data Rows : 2001\nwrite: too many args.\nngspice-46 done\n");
        QVERIFY(SimulationRun::logContainsError(log));
        const QList<qucs_s::simlog::Problem> problems = qucs_s::simlog::problems(log);
        QCOMPARE(problems.size(), 1);
        QCOMPARE(problems.first().severity, qucs_s::simlog::Problem::Error);
        QVERIFY(problems.first().message.contains(QStringLiteral("too many args")));
        // (Too few: no error - an empty print of a DC point said so, and the
        // run's results were whole.)
        QVERIFY(qucs_s::simlog::problems(QStringLiteral("print: too few args.\n")).isEmpty());
        QVERIFY(qucs_s::simlog::problems(QStringLiteral("Note: the args are many\n")).isEmpty());
        QVERIFY(!SimulationRun::logContainsError(QStringLiteral("Note: the args are many\n")));
    }

    // An undo step keeps its text packed and gives it back whole; undo and
    // redo through many steps of a large schematic come back to each text.
    void undoStepsArePackedAndComeBackWhole()
    {
        const QString text = QStringLiteral("i0") + labelledChain(500);
        UndoState state(text);
        QCOMPARE(state.text(), text);
        QCOMPARE(state.at(0), QLatin1Char('i'));
        QCOMPARE(state.at(1), QLatin1Char('0'));
        state.replace(1, 1, QLatin1Char('*'));
        QCOMPARE(state.at(1), QLatin1Char('*'));
        QCOMPARE(state.text().mid(1), QStringLiteral("*") + text.mid(2));
        QVERIFY2(state.bytes() * 5 < text.size(), qPrintable(QString::number(state.bytes())));
        // (Its first two characters always: a shorter text made up to them.)
        QVERIFY(UndoState(QStringLiteral("x")).text().startsWith(QLatin1Char('x')));
        QCOMPARE(UndoState(QStringLiteral("x")).text().size(), 2);
        QCOMPARE(UndoState(QString()).text().size(), 2);

        QVERIFY(write("undo.sch", labelledChain(2000).toUtf8()));
        Schematic sch(nullptr, dir.filePath("undo.sch"));
        QVERIFY(sch.load());   // (its undo stack begun with it)
        QStringList texts{sch.snapshot()};
        for (int k = 0; k < 10; ++k) {
            Component* r = sch.getComponentByName(QStringLiteral("R%1").arg(100 + k));
            QVERIFY(r != nullptr);
            r->Props.first()->Value = QStringLiteral("%1k").arg(k + 2);
            sch.setChanged(true, true);
            texts << sch.snapshot();
        }
        QCOMPARE(sch.undoCount(), 11);
        qsizetype kept = 0;
        for (const UndoState& s : sch.undoStacks().action) kept += s.bytes();
        QVERIFY2(kept * 5 < texts.first().size() * 2 * 11, qPrintable(QString::number(kept)));
        for (int k = 10; k > 0; --k) {
            QCOMPARE(sch.undoState(k).mid(2), sch.undoStates().at(k).mid(2));
            QVERIFY(sch.undo());
            QCOMPARE(sch.snapshot(), texts.at(k - 1));
        }
        QVERIFY(!sch.undo());
        for (int k = 1; k <= 10; ++k) {
            QVERIFY(sch.redo());
            QCOMPARE(sch.snapshot(), texts.at(k));
        }
    }

    // A dataset read where it lies (mapped): its values whatever its
    // headers claim, none when it says "nan", the same once the file is
    // gone.
    void aDatasetIsReadAsItIs()
    {
        QByteArray text = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 1000000000>\n";
        for (int k = 0; k < 5; ++k) text += "  " + QByteArray::number(k * 1e-6) + "\n";
        text += "</indep>\n<indep x nan>\n  1\n  2\n</indep>\n<dep v time x>\n";
        for (int k = 0; k < 10; ++k) text += "  " + QByteArray::number(k) + "\n";
        text += "</dep>\n";
        QVERIFY(write("claims.dat", text));
        const qint64 before = heapInUse();
        {
            qucs_s::dataset::Dataset data;
            QString error;
            QVERIFY2(data.read(dir.filePath("claims.dat"), &error), qPrintable(error));
            if (before >= 0)   // (a header of 10^9 values: no room made for them)
                QVERIFY2(heapInUse() - before < 8 * 1024 * 1024, qPrintable(QString::number(heapInUse() - before)));
            QFile::remove(dir.filePath("claims.dat"));
            const auto* v = data.find(QStringLiteral("v"));
            QVERIFY(v != nullptr);
            QCOMPARE(v->re.size(), 10);
            QCOMPARE(v->re.at(9), 9.0);
            QCOMPARE(data.find(QStringLiteral("time"))->re.size(), 5);
        }
        QVERIFY(write("notone.dat", "  <Qucs Datase"));
        qucs_s::dataset::Dataset none;
        QVERIFY(!none.read(dir.filePath("notone.dat")));
        QVERIFY(write("empty.dat", ""));
        QVERIFY(!none.read(dir.filePath("empty.dat")));
    }

    // A graph's data read from the mapped file, which is left as it was;
    // a file that ends at a page's end is read to its last byte and no
    // further.
    void aGraphReadsTheFileWithoutChangingIt()
    {
        const int simulator = QucsSettings.DefaultSimulator;
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QByteArray text = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 3>\n  0\n  1\n  2\n</indep>\n"
                          "<dep tran.v(out) time>\n  5\n  6\n  7\n</dep>\n";
        // Up to 16 KiB exactly (a page on Apple's arm64, four on others).
        text.insert(text.indexOf("<dep"), QByteArray(16384 - text.size(), ' '));
        QCOMPARE(text.size(), 16384);
        QVERIFY(write("page.dat.ngspice", text));
        {
            RectDiagram d;
            auto* g = new Graph(&d, QStringLiteral("ngspice/tran.v(out)"));
            d.Graphs.append(g);
            QVERIFY(g->loadDatFile(dir.filePath("page.dat")) > 0);
            QCOMPARE(g->count(0), size_t(3));
            QVERIFY(g->cPointsY != nullptr);
            QCOMPARE(g->cPointsY[0], 5.0);
            QCOMPARE(g->cPointsY[4], 7.0);   // (real, imaginary)
        }
        QCOMPARE(read("page.dat.ngspice"), text);
        QVERIFY(write("nothing.dat.ngspice", ""));
        {
            RectDiagram d;
            auto* g = new Graph(&d, QStringLiteral("ngspice/tran.v(out)"));
            d.Graphs.append(g);
            QCOMPARE(g->loadDatFile(dir.filePath("nothing.dat")), 0);
        }
        QucsSettings.DefaultSimulator = simulator;
    }
};

QTEST_MAIN(TestScaleAndMemory)
#include "test_scale_and_memory.moc"
