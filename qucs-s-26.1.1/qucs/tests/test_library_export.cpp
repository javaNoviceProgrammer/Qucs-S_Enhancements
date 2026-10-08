/*
 * Libraries made of a project's subcircuits and brought to another circuit
 * whole (the bug hunt of 7 October 2026): a part named as its file - a
 * subcircuit in a folder of the project, a name with a dot -; a hierarchical
 * subcircuit's model whole, its inner subcircuits named for the part, so
 * that two libraries' "inner", and the circuit's own, stay three; a renamed
 * library's parts called as the netlist names them; the SPICE files a
 * subcircuit uses taken in under names of their own, with the files they
 * include, a .inc and a .mod included where the part is used; a library
 * part placed in a library subcircuit in its Qucs model too; a library
 * replaced only when the new one is made, the old one to the trash; a
 * folder of the library's name that is no library's left alone; the dialog
 * choosing its subcircuits afresh, and saving one with changes first; the
 * commands of a library part's SPICE text found; the converter's library of
 * nothing refused.
 *
 * Netlists are written, not simulated: no simulator is needed.
 */
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "config.h"
#include "erc.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucsdoc.h"
#include "schematic.h"
#include "settings.h"
#include "components/component.h"
#include "components/libcomp.h"
#include "dialogs/importdialog.h"
#include "dialogs/librarydialog.h"
#include "dialogs/libraryexportdialog.h"
#include "dialogs/settingsdialog.h"
#include "spicecomponents/sp_libraryexport.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QString write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(bytes);
    return path;
}

QString read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QByteArray schematic(const QString& lines)
{
    return (QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n") + lines
            + QStringLiteral("</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n"))
        .toUtf8();
}

const QString kPorts = QStringLiteral("  <Port P1 1 220 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                                      "  <Port P2 1 280 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n");

QString resistor(const QString& name, const QString& value, int x, int y, int turn)
{
    return QStringLiteral("  <R %1 1 %2 %3 -26 15 0 %4 \"%5\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
        .arg(name).arg(x).arg(y).arg(turn).arg(value);
}

// P1 -R1- P2, R2 from P2 to ground.
QByteArray divider(const QString& r1, const QString& r2)
{
    return schematic(kPorts + resistor("R1", r1, 250, 100, 0) + resistor("R2", r2, 280, 130, 1)
                     + QStringLiteral("  <GND * 1 280 160 0 0 0 0>\n"));
}

// Two ports and a subcircuit \a file (wired or not: the names are what count).
QByteArray wrapper(const QString& file)
{
    return schematic(kPorts + QStringLiteral("  <Sub SUB1 1 250 300 -26 21 0 0 \"%1\" 1>\n").arg(file));
}

QString libPart(const QString& name, const QString& lib, const QString& comp, int x, int y)
{
    return QStringLiteral("  <Lib %1 1 %2 %3 13 10 0 0 \"%4\" 0 \"%5\" 0>\n").arg(name).arg(x).arg(y).arg(lib, comp);
}

QString spLib(const QString& name, const QString& file, const QString& device, int x, int y, bool active = true)
{
    return QStringLiteral("  <SpLib %1 %2 %3 %4 -26 92 0 0 \"%5\" 1 \"%6\" 1 \"auto\" 0 \"\" 0 \"\" 0>\n")
        .arg(name).arg(active ? 1 : 0).arg(x).arg(y).arg(file, device);
}

QString include(const QString& name, const QString& file, int x, int y, bool active = true)
{
    return QStringLiteral("  <SpiceInclude %1 %2 %3 %4 -34 16 0 0 \"%5\" 1 \"\" 0 \"\" 0 \"\" 0 \"\" 0>\n")
        .arg(name).arg(active ? 1 : 0).arg(x).arg(y).arg(file);
}

QByteArray subckt(const QString& name, const QString& r2)
{
    return QStringLiteral("* %1\n.subckt %1 1 2\nR1 1 2 1k\nR2 2 0 %2\n.ends %1\n").arg(name, r2).toUtf8();
}

// The .SUBCKT names \a netlist defines, in order.
QStringList defined(const QString& netlist)
{
    static const QRegularExpression subckt(QStringLiteral("^\\s*\\.SUBCKT\\s+(\\S+)"),
                                           QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    QStringList names;
    for (auto it = subckt.globalMatch(netlist); it.hasNext();) names << it.next().captured(1);
    return names;
}

// The .Def names of a Qucs netlist.
QStringList qucsDefined(const QString& netlist)
{
    static const QRegularExpression def(QStringLiteral("^\\s*\\.Def:(\\S+)"), QRegularExpression::MultilineOption);
    QStringList names;
    for (auto it = def.globalMatch(netlist); it.hasNext();)
        if (const QString n = it.next().captured(1); n != QLatin1String("End")) names << n;
    return names;
}

int lines(const QString& text, const QString& pattern)
{
    const QRegularExpression re(pattern, QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    int n = 0;
    for (auto it = re.globalMatch(text); it.hasNext(); it.next()) ++n;
    return n;
}

// The lines of the subcircuit \a name in \a netlist, its .SUBCKT to its .ENDS.
QString block(const QString& netlist, const QString& name)
{
    const qsizetype at = netlist.indexOf(QStringLiteral(".SUBCKT %1 ").arg(name));
    if (at < 0) return {};
    const qsizetype end = netlist.indexOf(QStringLiteral(".ENDS"), at);
    return netlist.mid(at, end < 0 ? -1 : end - at);
}

bool hasDuplicates(QStringList names)
{
    for (QString& n : names) n = n.toLower();
    return QSet<QString>(names.cbegin(), names.cend()).size() != names.size();
}

} // namespace

class TestLibraryExport : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString workspace, userLib, trash;

    // A project NAME of \a files, the one Qucs-S works in.
    QString project(const QString& name, const QHash<QString, QByteArray>& files)
    {
        const QString path = workspace + "/" + name + "_prj";
        QDir().mkpath(path);
        for (auto it = files.begin(); it != files.end(); ++it) write(path + "/" + it.key(), it.value());
        QucsSettings.QucsWorkDir.setPath(path);
        return path;
    }

    // A library made as create_library makes it; the messages, or "not
    // made: " and why.
    QString make(QucsApp& app, const QString& name, const QStringList& subcircuits, const QString& folder, bool replace = false,
                 QStringList* trashed = nullptr, bool ground = false)
    {
        LibraryDialog dialog(&app);
        dialog.fillSchematicList(subcircuits);
        LibraryDialog::Request request;
        request.name = name;
        request.subcircuits = subcircuits;
        request.folder = folder;
        request.replace = replace;
        request.embedVerilogA = true;
        request.groundPin = ground;
        QString log, error;
        if (!dialog.create(request, &log, &error, trashed)) return QStringLiteral("not made: ") + error + "\n" + log;
        return log;
    }

    // The ngspice netlist of \a file.
    QString netlistOf(const QString& file)
    {
        Schematic sch(nullptr, file);
        if (!sch.load()) return {};
        Ngspice kernel(&sch);
        kernel.setWorkdir(dir.filePath("kernel"));
        kernel.SaveNetlist(dir.filePath("kernel/net.cir"), false);
        return read(dir.filePath("kernel/net.cir"));
    }

    // The Qucsator netlist of \a file.
    QString qucsNetlistOf(const QString& file)
    {
        Schematic sch(nullptr, file);
        if (!sch.load()) return {};
        const int simulator = QucsSettings.DefaultSimulator;
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
        QString text;
        QTextStream stream(&text);
        QStringList collect;
        QPlainTextEdit errors;
        const int ports = sch.prepareNetlist(stream, collect, &errors);
        stream << '\n';
        sch.createNetlist(stream, ports);
        QucsSettings.DefaultSimulator = simulator;
        stream.flush();
        return text;
    }

    int trashed() const { return QDir(trash).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(); }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        trash = qEnvironmentVariable("QUCS_TRASH_DIR");
        QVERIFY(!trash.isEmpty());
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QDir().mkpath(dir.filePath("kernel"));
        QucsSettings.S4Qworkdir = dir.filePath("kernel");
        workspace = dir.filePath("workspace");
        userLib = workspace + "/user_lib";
        QVERIFY(QDir().mkpath(userLib));
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
        QucsSettings.LibraryPaths.clear();
    }

    // A part is its file's name without .sch: sub/deep.sch's is deep (it
    // was "sub/deep", its subcircuit deep, called as Odd_sub_deep: an
    // unknown subcircuit), div.v2.sch's div.v2 (it was div, a second part
    // div no one could place). Two of one name are refused.
    void partsAreNamedAsTheirFiles()
    {
        const QString p = project("names", {{"div.sch", divider("1k", "3k")}, {"div.v2.sch", divider("3k", "1k")},
                                            {"sub/deep.sch", divider("2k", "2k")}});
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "names";
        QString log = make(app, "Odd", {"div.sch", "div.v2.sch", "sub/deep.sch"}, userLib);
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        const QString lib = read(userLib + "/Odd.lib");
        QVERIFY2(lib.contains("<Component div>\n") && lib.contains("<Component div.v2>\n") && lib.contains("<Component deep>\n"),
                 qPrintable(lib.left(400)));
        QCOMPARE(defined(lib), (QStringList{"Odd_div", "Odd_div_v2", "Odd_deep"}));
        write(p + "/use.sch", schematic(libPart("X1", "Odd", "div", 300, 100) + libPart("X2", "Odd", "div.v2", 300, 300)
                                        + libPart("X3", "Odd", "deep", 300, 500)));
        const QString net = netlistOf(p + "/use.sch");
        for (const QString& sub : {QStringLiteral("Odd_div"), QStringLiteral("Odd_div_v2"), QStringLiteral("Odd_deep")}) {
            QVERIFY2(defined(net).contains(sub), qPrintable(net));
            QVERIFY2(lines(net, QStringLiteral("^XX\\d.* %1$").arg(sub)) == 1, qPrintable(net));
        }
        // Two that would be one part, or one SPICE subcircuit: refused, nothing written.
        write(p + "/zz/div.sch", divider("1k", "1k"));
        log = make(app, "Twice", {"div.sch", "zz/div.sch"}, userLib);
        QVERIFY2(log.startsWith("not made") && log.contains("would both be the part div"), qPrintable(log));
        QVERIFY(!QFileInfo::exists(userLib + "/Twice.lib"));
        QVERIFY(LibraryDialog::nameClash("L", {"a_b.sch", "a.b.sch"}).contains("the SPICE subcircuit L_a_b"));
        QVERIFY(LibraryDialog::nameClash("L", {"Amp.sch", "sub/amp.sch"}).contains("the part amp"));
        QVERIFY(LibraryDialog::nameClash("L", {"a.sch", "sub/b.sch", "c.v2.sch"}).isEmpty());
        QCOMPARE(LibraryDialog::partName("sub/deep.sch"), QStringLiteral("deep"));
        QCOMPARE(LibraryDialog::partName("div.v2.sch"), QStringLiteral("div.v2"));
        app.ProjName.clear();
    }

    // A part made of a subcircuit placing another has its SPICE model
    // whole: the first .SUBCKT was written on the <Spice> tag's line, and
    // read without it (an .ENDS too many: ngspice stopped). A library made
    // so before is read whole too.
    void hierarchicalPartsHaveTheirWholeModel()
    {
        const QString p = project("hier", {{"inner.sch", divider("1k", "1k")}, {"outer.sch", wrapper("inner.sch")},
                                           {"outer3.sch", wrapper("outer.sch")}});
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "hier";
        const QString log = make(app, "H", {"outer.sch", "outer3.sch"}, userLib);
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        const QString lib = read(userLib + "/H.lib");
        QVERIFY2(!lib.contains(QRegularExpression(QStringLiteral("<Spice>[^\\n]*\\.SUBCKT"))), qPrintable(lib));
        write(p + "/use.sch", schematic(libPart("X1", "H", "outer", 300, 100) + libPart("X2", "H", "outer3", 300, 300)));
        QString net = netlistOf(p + "/use.sch");
        QCOMPARE(lines(net, "^\\.SUBCKT "), lines(net, "^\\.ENDS"));
        QStringList names = defined(net);
        QVERIFY2(!hasDuplicates(names), qPrintable(names.join(' ')));
        for (const char* name : {"H_outer", "H_outer__inner", "H_outer3", "H_outer3__outer", "H_outer3__inner"})
            QVERIFY2(names.contains(name), qPrintable(names.join(' ')));
        QVERIFY2(block(net, "H_outer").contains("H_outer__inner"), qPrintable(net));
        QVERIFY2(block(net, "H_outer3__outer").contains("H_outer3__inner"), qPrintable(net));
        // Qucsator: each part's own copy of the inner subcircuit both include
        // (inner.sch.lst), every subcircuit called defined.
        const QString qnet = qucsNetlistOf(p + "/use.sch");
        const QStringList defs = qucsDefined(qnet);
        QVERIFY2(!hasDuplicates(defs), qPrintable(qnet));
        for (const char* name : {"H_outer", "H_outer__inner", "H_outer3", "H_outer3__outer", "H_outer3__inner"})
            QVERIFY2(defs.contains(name), qPrintable(qnet));
        static const QRegularExpression called(QStringLiteral("Type=\"([^\"]*)\""));
        for (auto it = called.globalMatch(qnet); it.hasNext();) {
            const QString type = it.next().captured(1);
            QVERIFY2(defs.contains(type), qPrintable(type + " is not defined:\n" + qnet));
        }

        // One written as before 26.1.6: the first .SUBCKT on the tag's line.
        write(userLib + "/Old.lib", QString(lib).replace("<Spice>\n", "<Spice>").toUtf8());
        QVERIFY(QString(read(userLib + "/Old.lib")).contains("<Spice>.SUBCKT inner"));
        write(p + "/old.sch", schematic(libPart("X1", "Old", "outer", 300, 100)));
        net = netlistOf(p + "/old.sch");
        QCOMPARE(lines(net, "^\\.SUBCKT "), lines(net, "^\\.ENDS"));
        QVERIFY2(defined(net).contains("Old_outer__inner") && defined(net).contains("Old_outer"), qPrintable(net));
        app.ProjName.clear();
    }

    // Two libraries' parts each made of a subcircuit placing an "inner" of
    // their own, and the circuit's own inner.sch: three subcircuits (ngspice
    // took the first "inner" for all of them, and said only "redefinition
    // ignored"; Qucsator refused three definitions).
    void innerSubcircuitsStayApart()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        project("la", {{"inner.sch", divider("1k", "1k")}, {"outer.sch", wrapper("inner.sch")}});
        app.ProjName = "la";
        QVERIFY(make(app, "LA", {"outer.sch"}, userLib).contains("Successfully"));
        project("lb", {{"inner.sch", divider("1k", "3k")}, {"outer.sch", wrapper("inner.sch")}});
        app.ProjName = "lb";
        QVERIFY(make(app, "LB", {"outer.sch"}, userLib).contains("Successfully"));
        const QString p = project("lu", {{"inner.sch", divider("3k", "1k")}});
        app.ProjName = "lu";
        write(p + "/use.sch", schematic(libPart("X1", "LA", "outer", 300, 100) + libPart("X2", "LB", "outer", 300, 300)
                                        + QStringLiteral("  <Sub SUB9 1 300 500 -26 21 0 0 \"inner.sch\" 1>\n")));
        const QString net = netlistOf(p + "/use.sch");
        const QStringList names = defined(net);
        QVERIFY2(!hasDuplicates(names), qPrintable(net));
        QVERIFY2(names.contains("LA_outer__inner") && names.contains("LB_outer__inner") && names.contains("inner"), qPrintable(net));
        QVERIFY2(block(net, "LA_outer__inner").contains("R2 0 P2  1K"), qPrintable(net));
        QVERIFY2(block(net, "LB_outer__inner").contains("R2 0 P2  3K"), qPrintable(net));
        QVERIFY2(block(net, "inner").contains("R1 P1 P2  3K"), qPrintable(net));
        QVERIFY2(block(net, "LB_outer").contains("LB_outer__inner"), qPrintable(net));
        const QString qnet = qucsNetlistOf(p + "/use.sch");
        const QStringList defs = qucsDefined(qnet);
        QVERIFY2(!hasDuplicates(defs) && defs.contains("LA_outer__inner") && defs.contains("LB_outer__inner") && defs.contains("inner"),
                 qPrintable(qnet));
        QVERIFY2(qnet.contains("Type=\"LB_outer__inner\""), qPrintable(qnet));
        app.ProjName.clear();
    }

    // The renaming itself: the own subcircuit found by its name, the name
    // the library was made under, else last; a call's subcircuit is the word
    // before its parameters, in any case, over its continuation lines; a node
    // of the name and a vendor's subcircuit are left as they are.
    void innerNamesAreScopedByTheirWords()
    {
        const QString spice = ".SUBCKT INNER a b\nR1 a b 1k\n.ENDS INNER\n"
                              ".SUBCKT Mine_part p n r=1\n"
                              "XU1 p n inner r=1k\n"
                              "XU2 p n inner r = 2k\n"
                              "XU3 inner n inner params: r=3\n"
                              "xu4 p\n+ n inner\n+ r={ 2 * r }\n"
                              "XV p n VENDOR\n"
                              ".ENDS\n";
        const QString scoped = LibComp::scopedSpice(spice, "Lib_part", "Mine_part");
        QCOMPARE(scoped, QStringLiteral(".SUBCKT Lib_part__INNER a b\nR1 a b 1k\n.ENDS Lib_part__INNER\n"
                                        ".SUBCKT Lib_part p n r=1\n"
                                        "XU1 p n Lib_part__INNER r=1k\n"
                                        "XU2 p n Lib_part__INNER r = 2k\n"
                                        "XU3 inner n Lib_part__INNER params: r=3\n"
                                        "xu4 p\n+ n Lib_part__INNER\n+ r={ 2 * r }\n"
                                        "XV p n VENDOR\n"
                                        ".ENDS\n"));
        // Its own by name: kept, the last one renamed like any other.
        QCOMPARE(LibComp::scopedSpice(".SUBCKT Lib_part a b\nX1 a b helper\n.ENDS\n.SUBCKT helper a b\n.ENDS\n", "Lib_part", ""),
                 QStringLiteral(".SUBCKT Lib_part a b\nX1 a b Lib_part__helper\n.ENDS\n.SUBCKT Lib_part__helper a b\n.ENDS\n"));
        // A comment after a call: its words are not the subcircuit (the 555
        // timer's "X1 6 5 22 comparator5 ; the reset comparator").
        QCOMPARE(LibComp::scopedSpice(".subckt comp5 1 2 5\n.ends\n.subckt L_p a b\nX1 6 5 22 comp5 ; the reset comparator\n"
                                      "X2 a b comp5;set\nX3 a b comp5 $ a note\n.ends\n", "L_p", ""),
                 QStringLiteral(".subckt L_p__comp5 1 2 5\n.ends\n.subckt L_p a b\nX1 6 5 22 L_p__comp5 ; the reset comparator\n"
                                "X2 a b L_p__comp5;set\nX3 a b L_p__comp5 $ a note\n.ends\n"));
        // Not its name, but the one the library was made under, and not last.
        QCOMPARE(LibComp::scopedSpice(".SUBCKT Mine_part a b\nX1 a b helper\n.ENDS\n.SUBCKT helper a b\n.ENDS\n", "Lib_part", "Mine_part"),
                 QStringLiteral(".SUBCKT Lib_part a b\nX1 a b Lib_part__helper\n.ENDS\n.SUBCKT Lib_part__helper a b\n.ENDS\n"));
        // None of them its name: the last is its own.
        QCOMPARE(LibComp::scopedSpice(".SUBCKT x a\n.ENDS\n.SUBCKT y a\nX1 a x\n.ENDS\n", "L_p", "L_q"),
                 QStringLiteral(".SUBCKT L_p__x a\n.ENDS\n.SUBCKT L_p a\nX1 a L_p__x\n.ENDS\n"));
        QCOMPARE(LibComp::scopedSpice("R1 a b 1k\n", "L_p", "L_p"), QStringLiteral("R1 a b 1k\n"));   // no subcircuit
        const QString model = ".Def:inner a b\nR:R1 a b R=\"1k\"\n.Def:End\n.Def:Old_part p n\nSub:SUB1 p n Type=\"inner\" r=\"1\"\n"
                              "Sub:SUB2 p n Type=\"vendor\"\n.Def:End\n";
        QCOMPARE(LibComp::scopedQucsModel(model, "New_part", "Old_part"),
                 QStringLiteral(".Def:New_part__inner a b\nR:R1 a b R=\"1k\"\n.Def:End\n.Def:New_part p n\n"
                                "Sub:SUB1 p n Type=\"New_part__inner\" r=\"1\"\nSub:SUB2 p n Type=\"vendor\"\n.Def:End\n"));
    }

    // A library file renamed (a name of its own, as also_named advises): its
    // parts called by the new name, which the netlist calls (unknown
    // subcircuit AmpsTeam_div, under ngspice and Qucsator).
    void aRenamedLibrarysPartsAreCalledRight()
    {
        const QString p = project("ren", {{"inner.sch", divider("1k", "1k")}, {"outer.sch", wrapper("inner.sch")}});
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "ren";
        QVERIFY(make(app, "Amps", {"outer.sch"}, p).contains("Successfully"));
        QVERIFY(QFile::rename(p + "/Amps.lib", userLib + "/Team.lib"));
        if (QFileInfo::exists(p + "/Amps")) QVERIFY(QDir().rename(p + "/Amps", userLib + "/Team"));
        write(p + "/use.sch", schematic(libPart("X1", "Team", "outer", 300, 100)));
        const QString net = netlistOf(p + "/use.sch");
        QVERIFY2(defined(net).contains("Team_outer") && defined(net).contains("Team_outer__inner"), qPrintable(net));
        QVERIFY2(lines(net, "^XX1 .* Team_outer$") == 1, qPrintable(net));
        const QString qnet = qucsNetlistOf(p + "/use.sch");
        QVERIFY2(qucsDefined(qnet).contains("Team_outer") && qnet.contains("Type=\"Team_outer\""), qPrintable(qnet));
        app.ProjName.clear();
    }

    // The SPICE files of a library's subcircuits: two of one name both kept
    // (other/dev.lib in a folder of its own - the second overwrote the
    // first), with the files they include; a .inc and a .mod included where
    // the part is used (only .cir, .ckt, .lib and .sp were); a deactivated
    // .INCLUDE's file not taken in.
    void spiceFilesKeepTheirNamesAndAreIncluded()
    {
        const QString p = project("sp", {{"models/dev.lib", "* a\n.include \"sub/x.inc\"\n.subckt DEVA 1 2\nXQ 1 2 XI\n.ends\n"},
                                         {"models/sub/x.inc", subckt("XI", "1k")},
                                         {"other/dev.lib", subckt("DEVB", "3k")},
                                         {"vendor.inc", subckt("DEVC", "1k")},
                                         {"vendor.mod", subckt("DEVD", "4k")},
                                         {"off.lib", subckt("OFF", "1k")}});
        write(p + "/a.sch", schematic(kPorts + spLib("X1", "models/dev.lib", "DEVA", 300, 200)));
        write(p + "/b.sch", schematic(kPorts + spLib("X1", "other/dev.lib", "DEVB", 300, 200)));
        write(p + "/c.sch", schematic(kPorts + spLib("X1", "vendor.inc", "DEVC", 300, 200)));
        write(p + "/e.sch", schematic(kPorts + include("SpiceInclude1", "vendor.mod", 300, 300) + include("SpiceInclude2", "off.lib", 300, 400, false)
                                      + spLib("X2", "off2.lib", "OFF2", 300, 500, false)));
        write(p + "/off2.lib", subckt("OFF2", "1k"));
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "sp";
        const QString log = make(app, "Sp", {"a.sch", "b.sch", "c.sch", "e.sch"}, userLib);
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        QVERIFY(read(userLib + "/Sp/dev.lib").contains("DEVA"));
        QVERIFY(read(userLib + "/Sp/sub/x.inc").contains("XI"));
        QVERIFY(read(userLib + "/Sp/other/dev.lib").contains("DEVB"));
        QVERIFY(QFileInfo::exists(userLib + "/Sp/vendor.inc") && QFileInfo::exists(userLib + "/Sp/vendor.mod"));
        QVERIFY(!QFileInfo::exists(userLib + "/Sp/off.lib") && !QFileInfo::exists(userLib + "/Sp/off2.lib"));
        const QString lib = read(userLib + "/Sp.lib");
        QVERIFY2(lib.contains("<SpiceAttach \"dev.lib\">") && lib.contains("<SpiceAttach \"other/dev.lib\">"), qPrintable(lib));
        write(p + "/use.sch", schematic(libPart("X1", "Sp", "a", 300, 100) + libPart("X2", "Sp", "b", 300, 300)
                                        + libPart("X3", "Sp", "c", 300, 500) + libPart("X4", "Sp", "e", 300, 700)));
        const QString net = netlistOf(p + "/use.sch");
        for (const QString& f : {QStringLiteral("Sp/dev.lib"), QStringLiteral("Sp/other/dev.lib"), QStringLiteral("Sp/vendor.inc"),
                                 QStringLiteral("Sp/vendor.mod")})
            QVERIFY2(lines(net, QStringLiteral("^\\.INCLUDE \".*/user_lib/%1\"$").arg(QRegularExpression::escape(f))) == 1, qPrintable(f + "\n" + net));
        app.ProjName.clear();
    }

    // A library part placed in a library's subcircuit: its Qucs model goes
    // into the new library (it was skipped - "WARNING: Skipping library
    // component" - and Qucsator had no definition of it).
    void aLibraryPartInsideIsInTheQucsModel()
    {
        const QString p = project("nest", {{"div.sch", divider("1k", "3k")}});
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "nest";
        QVERIFY(make(app, "Keep", {"div.sch"}, userLib).contains("Successfully"));
        write(p + "/wrap.sch", schematic(kPorts + libPart("X1", "Keep", "div", 300, 200)));
        const QString log = make(app, "Other", {"wrap.sch"}, userLib);
        QVERIFY2(log.contains("Successfully") && !log.contains("Skipping"), qPrintable(log));
        QVERIFY2(read(userLib + "/Other.lib").contains("<ModelIncludes \"Keep_div.lst\">"), qPrintable(read(userLib + "/Other.lib")));
        QVERIFY(read(userLib + "/Other/Keep_div.lst").contains(".Def:Keep_div"));
        write(p + "/use.sch", schematic(libPart("X1", "Other", "wrap", 300, 100) + libPart("X2", "Keep", "div", 300, 300)));
        const QString qnet = qucsNetlistOf(p + "/use.sch");
        const QStringList defs = qucsDefined(qnet);
        QVERIFY2(!hasDuplicates(defs) && defs.contains("Other_wrap__Keep_div") && defs.contains("Keep_div"), qPrintable(qnet));
        QVERIFY2(qnet.contains("Type=\"Other_wrap__Keep_div\""), qPrintable(qnet));
        const QString net = netlistOf(p + "/use.sch");
        QVERIFY2(!hasDuplicates(defined(net)) && defined(net).contains("Other_wrap__Keep_div"), qPrintable(net));
        app.ProjName.clear();
    }

    // Replaced, a library stays there until the new one is made: read while
    // it is made (a subcircuit of it placing one of its parts - that part's
    // model was left empty), there as it was when the new one cannot be
    // made (it was deleted, by the dialog for good), and to the trash once
    // the new one is in place. A library not made leaves no folder.
    void aLibraryIsReplacedOnlyByOneMade()
    {
        const QString p = project("rep", {{"div.sch", divider("1k", "3k")}, {"outer.sch", wrapper("gone.sch")}});
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "rep";
        QVERIFY(make(app, "Kept", {"div.sch"}, userLib).contains("Successfully"));
        write(p + "/wrap.sch", schematic(kPorts + libPart("X1", "Kept", "div", 300, 200)));
        const int before = trashed();
        QStringList went;
        QString log = make(app, "Kept", {"div.sch", "wrap.sch"}, userLib, true, &went);
        QVERIFY2(log.contains("Successfully") && log.contains("The library it replaced is in the trash."), qPrintable(log));
        QCOMPARE(went.size(), 1);
        QCOMPARE(trashed(), before + 1);
        const QString lib = read(userLib + "/Kept.lib");
        const qsizetype wrap = lib.indexOf("<Component wrap>");
        QVERIFY2(wrap > 0 && lib.mid(wrap).contains(".SUBCKT Kept_div"), qPrintable(lib));

        // One that cannot be made: the one there is as it was.
        log = make(app, "Kept", {"div.sch", "outer.sch"}, userLib, true);
        QVERIFY2(log.startsWith("not made") && log.contains("the one there is as it was"), qPrintable(log));
        QCOMPARE(read(userLib + "/Kept.lib"), lib);
        QCOMPARE(trashed(), before + 1);
        QVERIFY(!QFileInfo::exists(userLib + "/.Kept.qucs-new"));
        log = make(app, "Fresh", {"outer.sch"}, userLib);
        QVERIFY(log.startsWith("not made"));
        QVERIFY(!QFileInfo::exists(userLib + "/Fresh.lib") && !QFileInfo::exists(userLib + "/Fresh"));

        // No trash to move it to: create_library's replace is not made (it
        // says the old one is in the trash), the old one as it was.
        const QByteArray trashWas = qgetenv("QUCS_TRASH_DIR");
        write(dir.filePath("not-a-folder"), "a file");
        qputenv("QUCS_TRASH_DIR", dir.filePath("not-a-folder/trash").toUtf8());
        log = make(app, "Kept", {"div.sch"}, userLib, true);
        qputenv("QUCS_TRASH_DIR", trashWas);
        QVERIFY2(log.startsWith("not made") && log.contains("could not be moved to the trash"), qPrintable(log));
        QCOMPARE(read(userLib + "/Kept.lib"), lib);

        // The dialog's Rewrite? Yes, and it cannot be made: the same.
        LibraryDialog dialog(&app);
        dialog.fillSchematicList({"outer.sch"});
        dialog.findChild<QComboBox*>("destination")->setCurrentIndex(0);
        for (QCheckBox* box : dialog.findChildren<QCheckBox*>())
            if (box->text() == "Add subcircuit description") box->setChecked(false);
        dialog.findChild<QLineEdit*>()->setText("Kept");
        QStringList asked;
        QTimer yes;
        connect(&yes, &QTimer::timeout, [&asked] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                asked << box->text();
                if (QAbstractButton* b = box->button(QMessageBox::Yes)) b->click();
                else box->accept();
            }
        });
        yes.start(30);
        QMetaObject::invokeMethod(&dialog, "slotCreateNext");
        yes.stop();
        QVERIFY2(asked.size() == 1 && asked.first().contains("Rewrite?"), qPrintable(asked.join(" | ")));   // (no load error box)
        QCOMPARE(read(userLib + "/Kept.lib"), lib);
        QVERIFY(dialog.findChild<QPlainTextEdit*>()->toPlainText().contains("there is as it was"));
        app.ProjName.clear();
    }

    // A folder of the library's name that goes with no library of it (a
    // .lib taken away by hand, someone's own) is not taken away: the
    // library's files go into it, a model compiled from an earlier source
    // there removed. The project's own folders are not a library's.
    void aFolderOfItsNameIsLeftAlone()
    {
        const QString p = project("fold", {{"div.sch", divider("1k", "1k")}, {"amps/notes.txt", "mine"},
                                           {"spice.sch", schematic(kPorts + spLib("X1", "dev.lib", "DEV", 300, 200))},
                                           {"dev.lib", subckt("DEV", "1k")}});
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "fold";
        QString log = make(app, "amps", {"div.sch", "spice.sch"}, p);
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        QCOMPARE(read(p + "/amps/notes.txt"), QStringLiteral("mine"));
        QVERIFY(QFileInfo::exists(p + "/amps/dev.lib") && QFileInfo::exists(p + "/amps.lib"));
        QDir().mkpath(p + "/Scratch");
        log = make(app, "Scratch", {"div.sch"}, p);
        QVERIFY2(log.startsWith("not made") && log.contains("keeps for itself"), qPrintable(log));
        app.ProjName.clear();
    }

    // The dialog: Rewrite? No, another name, Next - each subcircuit once (it
    // was twice); a subcircuit open with changes saved first, when asked.
    void theDialogChoosesAfreshAndSavesFirst()
    {
        const QString p = project("dlg", {{"amp.sch", divider("1k", "1k")}});
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "dlg";
        QVERIFY(make(app, "Taken", {"amp.sch"}, userLib).contains("Successfully"));
        const auto dialogFor = [&app](LibraryDialog& dialog, const QString& name) {
            dialog.fillSchematicList({"amp.sch"});
            dialog.findChild<QComboBox*>("destination")->setCurrentIndex(0);
            for (QCheckBox* box : dialog.findChildren<QCheckBox*>())
                if (box->text() == "Add subcircuit description") box->setChecked(false);
            dialog.findChild<QLineEdit*>()->setText(name);
            Q_UNUSED(app);
        };
        {
            LibraryDialog dialog(&app);
            dialogFor(dialog, "Taken");
            QTimer no;
            connect(&no, &QTimer::timeout, [] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                    if (QAbstractButton* b = box->button(QMessageBox::No)) b->click();
            });
            no.start(30);
            QMetaObject::invokeMethod(&dialog, "slotCreateNext");
            no.stop();
            dialog.findChild<QLineEdit*>()->setText("Fresh2");
            QMetaObject::invokeMethod(&dialog, "slotCreateNext");
            QCOMPARE(int(read(userLib + "/Fresh2.lib").count("\n<Component ")), 1);
        }
        // Open, changed and not saved: saved first.
        QVERIFY(app.gotoPage(p + "/amp.sch"));
        Schematic* doc = nullptr;
        for (QucsDoc* d : app.allDocuments())
            if (misc::isSameFile(d->getDocName(), p + "/amp.sch")) doc = dynamic_cast<Schematic*>(d);
        QVERIFY(doc != nullptr);
        for (Component* c : doc->a_DocComps)
            if (c->Name == "R2") c->Props.at(0)->Value = "7k";
        doc->setDocChanged(true);
        LibraryDialog dialog(&app);
        dialogFor(dialog, "Saved");
        QString said;
        QTimer save;
        connect(&save, &QTimer::timeout, [&said] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); box != nullptr && box->objectName() == "saveSubcircuits") {
                said = box->text();
                for (QAbstractButton* b : box->buttons())
                    if (b->text() == "Save and Go On") b->click();
            }
        });
        save.start(30);
        QMetaObject::invokeMethod(&dialog, "slotCreateNext");
        save.stop();
        QVERIFY2(said.contains("amp.sch has changes that are not saved"), qPrintable(said));
        QVERIFY(!doc->getDocChanged());
        QVERIFY(read(p + "/amp.sch").contains("\"7k\""));
        QVERIFY2(read(userLib + "/Saved.lib").contains("7K"), qPrintable(read(userLib + "/Saved.lib")));
        for (QucsDoc* d : app.allDocuments()) d->setDocChanged(false);
        app.ProjName.clear();
    }

    // The commands a library part's SPICE text runs, and a SPICE file's (a
    // SPICE library part's, an .INCLUDE's and what they include, through a
    // subcircuit too) are found as the schematic's own are. Found only: the
    // lines are the bare keyword, and nothing is simulated.
    void commandsInLibrariesAreFound()
    {
        const QString p = project("cmd", {{"div.sch", divider("1k", "3k")}});
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "cmd";
        QVERIFY(make(app, "Plain", {"div.sch"}, userLib).contains("Successfully"));
        QString lib = read(userLib + "/Plain.lib");
        lib.replace("\"Plain\"", "\"Theirs\"").replace("Plain_", "Theirs_");
        lib.replace(".ENDS\n  </Spice>", ".ENDS\n.control\nshell\n.endc\n  </Spice>");
        write(userLib + "/Theirs.lib", lib.toUtf8());
        write(userLib + "/Attaching.lib", QString(read(userLib + "/Plain.lib")).replace("\"Plain\"", "\"Attaching\"")
                                              .replace("  </Spice>\n", "  </Spice>\n<SpiceAttach \"extra.lib\">\n").toUtf8());
        write(userLib + "/Attaching/extra.lib", "* extra\n.include \"deeper.inc\"\n");
        write(userLib + "/Attaching/deeper.inc", ".control\nsystem\n.endc\n");
        write(p + "/vendor.lib", "* vendor\n.control\n!\n.endc\n.subckt DEV 1 2\nR1 1 2 1k\n.ends\n");
        write(p + "/inc.lib", ".include \"more.inc\"\n");
        write(p + "/more.inc", ".control\nshell\n.endc\n");
        write(p + "/quiet.lib", ".control\necho\n.endc\nsystem\n");   // (outside a .control block: no command)
        write(p + "/sub.sch", schematic(kPorts + libPart("X1", "Theirs", "div", 300, 200) + spLib("X8", "vendor.lib", "DEV", 300, 400, false)));
        write(p + "/top.sch", schematic(QStringLiteral("  <Sub SUB1 1 300 100 -26 21 0 0 \"sub.sch\" 1>\n")
                                        + libPart("X3", "Attaching", "div", 300, 300) + spLib("X2", "vendor.lib", "DEV", 300, 500)
                                        + include("SpiceInclude1", "inc.lib", 300, 700) + include("SpiceInclude2", "quiet.lib", 300, 800)
                                        + spLib("X9", "vendor.lib", "DEV", 300, 900, false)));
        Schematic doc(nullptr, p + "/top.sch");
        QVERIFY(doc.load());
        const QStringList said = qucs_s::erc::commandsRun(&doc);
        const auto by = [&said](const QString& part) {
            for (const QString& s : said)
                if (s.startsWith(part + " brings SPICE text")) return s;
            return QString();
        };
        QVERIFY2(by("SUB1").contains("shell (in the library Theirs)") && !by("SUB1").contains("more lines"), qPrintable(said.join('\n')));
        QVERIFY2(by("X3").contains("system (in deeper.inc)"), qPrintable(said.join('\n')));
        QVERIFY2(by("X2").contains("! (in vendor.lib)"), qPrintable(said.join('\n')));
        QVERIFY2(by("SpiceInclude1").contains("shell (in more.inc)"), qPrintable(said.join('\n')));
        QVERIFY2(by("SpiceInclude2").isEmpty() && by("X9").isEmpty(), qPrintable(said.join('\n')));
        QCOMPARE(said.size(), 4);
        app.ProjName.clear();
    }

    // Convert Data File takes a vendor's SPICE library (.lib, .mod, .inc) as
    // SPICE, its output a Qucs library (it was read as a Qucs dataset, and
    // that output hidden).
    void aSpiceLibraryIsConvertedAsSpice()
    {
        for (const char* name : {"vendor.lib", "vendor.mod", "vendor.inc", "models.cir"}) {
            const QString file = write(dir.filePath(QStringLiteral("convert/") + name), "* models\n.model D1 D(IS=1n)\n");
            ImportDialog dialog(nullptr);
            auto* input = dialog.findChild<QLineEdit*>("importFile");
            auto* type = dialog.findChild<QComboBox*>("inputType");
            QVERIFY(input != nullptr && type != nullptr);
            input->setText(file);
            QVERIFY2(type->currentIndex() == 0, name);   // SPICE netlist
            QStringList outputs;   // (the output format's box has no name)
            for (QComboBox* box : dialog.findChildren<QComboBox*>())
                if (box != type && box->objectName() != "outputData") outputs << box->currentText();
            QVERIFY2(outputs == QStringList{"Qucs library"}, qPrintable(QString(name) + ": " + outputs.join(", ")));
        }
    }

    // Library Export: the section SPICE subcircuit, right after the netlist
    // sections, has it, with an icon of its own.
    void aLibraryExportHasASectionOfItsOwn()
    {
        Module::registerModules();   // (a QucsApp's destructor unregisters them)
        const QStringList sections = Category::getCategories();
        const qsizetype netlist = sections.indexOf(QObject::tr("SPICE netlist sections"));
        QVERIFY2(netlist >= 0, qPrintable(sections.join(", ")));
        QCOMPARE(sections.value(netlist + 1), QObject::tr("SPICE subcircuit"));
        const QList<Module*> modules = Category::getModules(QObject::tr("SPICE subcircuit"));
        QCOMPARE(modules.size(), 1);
        QString name;
        char* file = nullptr;
        std::unique_ptr<Element> e(modules.first()->info(name, file, true));
        QCOMPARE(name, QStringLiteral("Library Export"));
        auto* c = dynamic_cast<Component*>(e.get());
        QVERIFY(LibraryExport::is(c));
        QCOMPARE(misc::getIconPath(QString(file)), QStringLiteral(":bitmaps/svg/sp_libexport.svg"));
        QVERIFY(QFile::exists(misc::getIconPath(QString(file))));
        // Its settings: the defaults; and as given.
        QVERIFY(LibraryExport::settingsOf(c).isDefault());
        LibrarySettings s;
        s.alwaysLoadOSDI = true;
        s.modelCards = ".model m3 good\n+ r=2k";
        s.groundPin = LibrarySettings::Without;
        LibraryExport::setSettings(c, s);
        QCOMPARE(LibraryExport::settingsOf(c), s);
        QCOMPARE(c->getProperty("GroundPin")->Value, QStringLiteral("no"));
        c->getProperty("GroundPin")->Value = "With";   // (as typed)
        QCOMPARE(LibraryExport::settingsOf(c).groundPin, LibrarySettings::With);
        c->getProperty("GroundPin")->Value = "maybe";
        QCOMPARE(LibraryExport::settingsOf(c).groundPin, LibrarySettings::Default);
    }

    // A Library Export holds the schematic's library settings: placed, it
    // takes its Document Settings > Library (and undo gives them back);
    // one a schematic; off, it applies none; deleted, they go with it. The
    // Document Settings show and change it while it is there (a step undo
    // takes back), and its own dialog changes it. Saved in the part, not in
    // the file's properties; a file with both is read into the part. It
    // writes nothing into a netlist.
    void aLibraryExportHoldsTheLibrarySettings()
    {
        Module::registerModules();
        const unsigned undoSteps = QucsSettings.maxUndo;
        QucsSettings.maxUndo = 20;
        const auto restore = qScopeGuard([undoSteps] { QucsSettings.maxUndo = undoSteps; });
        const QString p = project("held", {{"div.sch", divider("1k", "3k")}});
        const QString file = p + "/div.sch";
        Schematic sch(nullptr, file);
        QVERIFY(sch.load());
        LibrarySettings own;
        own.alwaysLoadOSDI = true;
        own.modelCards = ".model m3 good";
        own.groundPin = LibrarySettings::With;
        sch.setLibrarySettings(own);
        sch.setChanged(true, true);   // (as Document Settings does)
        QCOMPARE(sch.librarySettings(), own);
        QVERIFY(sch.libraryGroundPin());
        QVERIFY(sch.save() >= 0);
        QVERIFY2(read(file).contains("\n  <LibraryGroundPin=1>\n") && read(file).contains("\n  <AlwaysLoadOSDI=1>\n"), qPrintable(read(file)));

        // Placed: it takes them.
        Component* part = new LibraryExport();
        QVERIFY(sch.refusedLibraryExport(part).isEmpty());
        part->setSchematic(&sch);
        sch.insertComponent(part);
        sch.setChanged(true, true);   // (as a click does)
        QVERIFY2(sch.takeLibraryNote().contains("LibExport1 took this schematic's Document Settings > Library"), "");
        QCOMPARE(sch.libraryExport(), part);
        QCOMPARE(LibraryExport::settingsOf(part), own);
        QCOMPARE(sch.librarySettings(), own);
        QVERIFY(sch.save() >= 0);
        QString saved = read(file);
        QVERIFY2(!saved.contains("<AlwaysLoadOSDI") && !saved.contains("<LibraryGroundPin") && !saved.contains("<ModelCards"),
                 qPrintable(saved));
        QVERIFY2(saved.contains("<LibraryExport LibExport1 1 "), qPrintable(saved));
        // Undone: the Document Settings have them again; redone: the part.
        QVERIFY(sch.undo());
        QVERIFY(sch.libraryExport() == nullptr);
        QCOMPARE(sch.librarySettings(), own);
        QVERIFY(sch.redo());
        part = sch.libraryExport();
        QVERIFY(part != nullptr);
        QCOMPARE(sch.librarySettings(), own);
        QVERIFY(sch.save() >= 0);
        QVERIFY2(!read(file).contains("<AlwaysLoadOSDI"), qPrintable(read(file)));
        // One a schematic.
        LibraryExport second;
        QVERIFY2(sch.refusedLibraryExport(&second).contains("LibExport1 holds this schematic's library settings already"), "");
        QVERIFY(sch.refusedLibraryExport(part).isEmpty());   // (itself)
        // Off: none applies; held all the same.
        part->isActive = COMP_IS_OPEN;
        QVERIFY(sch.librarySettings().isDefault());
        QCOMPARE(sch.heldLibrarySettings(), own);
        QVERIFY(!sch.libraryGroundPin());   // (Application Settings': off)
        QVERIFY(!sch.getAlwaysLoadOSDI());
        {
            SettingsDialog dialog(&sch);
            auto* held = dialog.findChild<QLabel*>("libraryHeldBy");
            QVERIFY(held != nullptr && !held->isHidden());
            QVERIFY2(held->text().contains("LibExport1") && held->text().contains("which is off"), qPrintable(held->text()));
        }
        part->isActive = COMP_IS_ACTIVE;

        // Document Settings > Library: the part's, changed in it.
        LibrarySettings changed = own;
        {
            SettingsDialog dialog(&sch);
            auto* held = dialog.findChild<QLabel*>("libraryHeldBy");
            QVERIFY2(held->text().startsWith("Held by LibExport1, the Library Export on the schematic"), qPrintable(held->text()));
            QVERIFY(dialog.findChild<QCheckBox*>("alwaysLoadOSDI")->isChecked());
            QCOMPARE(dialog.findChild<QPlainTextEdit*>("modelCards")->toPlainText(), own.modelCards);
            auto* pin = dialog.findChild<QComboBox*>("groundPin");
            QVERIFY(pin != nullptr);
            QCOMPARE(pin->currentData().toInt(), int(LibrarySettings::With));
            pin->setCurrentIndex(pin->findData(int(LibrarySettings::Without)));
            dialog.findChild<QCheckBox*>("alwaysModelCards")->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotApply"));
        }
        changed.groundPin = LibrarySettings::Without;
        changed.alwaysModelCards = true;
        QCOMPARE(LibraryExport::settingsOf(sch.libraryExport()), changed);
        QVERIFY(!sch.libraryGroundPin());
        QVERIFY(sch.getAlwaysModelCards());
        QVERIFY(sch.undo());   // (the dialog's step)
        QCOMPARE(sch.librarySettings(), own);
        QVERIFY(sch.libraryExport() != nullptr);
        QVERIFY(sch.redo());
        QCOMPARE(sch.librarySettings(), changed);
        // The setters change the part too.
        sch.setModelCards(".model m5 good");
        QCOMPARE(LibraryExport::settingsOf(sch.libraryExport()).modelCards, QStringLiteral(".model m5 good"));
        sch.setModelCards(changed.modelCards);

        // Its own dialog: a line that is no card refused, nothing applied;
        // a name taken refused; then applied.
        part = sch.libraryExport();
        {
            LibraryExportDialog dialog(part, &sch);
            QCOMPARE(dialog.options()->settings(), changed);
            dialog.options()->modelCards->setPlainText(".model m4 good\n.control\nshell ls\n.endc");
            QString said;
            QTimer::singleShot(0, [&said] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    said = box->text();
                    box->close();
                }
            });
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotOK"));
            QVERIFY2(said.contains("2: .control") && said.contains("Nothing was applied"), qPrintable(said));
            QCOMPARE(LibraryExport::settingsOf(part), changed);
            dialog.options()->modelCards->setPlainText(".model m4 good");
            dialog.findChild<QLineEdit*>("name")->setText("R1");
            said.clear();
            QTimer::singleShot(0, [&said] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    said = box->text();
                    box->close();
                }
            });
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotOK"));
            QVERIFY2(said.contains("There is a component named R1 already"), qPrintable(said));
            QCOMPARE(part->Name, QStringLiteral("LibExport1"));
            dialog.findChild<QLineEdit*>("name")->setText("Export");
            dialog.findChild<QCheckBox*>("shown")->setChecked(false);
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotOK"));
            QCOMPARE(dialog.result(), int(QDialog::Accepted));
        }
        changed.modelCards = ".model m4 good";
        QCOMPARE(part->Name, QStringLiteral("Export"));
        QCOMPARE(sch.librarySettings(), changed);
        for (const Property* prop : part->Props) QVERIFY2(!prop->display, qPrintable(prop->Name));

        // Saved and read back, in the part.
        QVERIFY(sch.save() >= 0);
        {
            Schematic again(nullptr, file);
            QVERIFY(again.load());
            QVERIFY(again.libraryExport() != nullptr);
            QCOMPARE(again.libraryExport()->Name, QStringLiteral("Export"));
            QCOMPARE(again.librarySettings(), changed);
        }
        // Nothing of it in a netlist: ngspice's, Qucsator's; nothing to
        // check, but that it is of a subcircuit.
        // (Its first line, a comment, names the file: the folder's name has "export" in it.)
        const QString net = netlistOf(file).section('\n', 1);
        QVERIFY2(!net.contains("Export", Qt::CaseInsensitive) && net.contains("R1 ") && net.contains("\n.model m4 good\n"), qPrintable(net));
        const QString qucs = qucsNetlistOf(file);
        QVERIFY2(!qucs.contains("LibraryExport") && !qucs.contains("Export:") && qucs.contains("R:R1 "), qPrintable(qucs));
        {
            QStringList incompatible;
            Ngspice kernel(&sch);
            QVERIFY2(kernel.checkSchematic(incompatible), qPrintable(incompatible.join(", ")));
            for (const qucs_s::erc::Issue& i : qucs_s::erc::check(&sch, false))
                QVERIFY2(i.component != "Export", qPrintable(i.message));
        }

        // Deleted: they go with it.
        sch.deleteComp(sch.libraryExport());
        QVERIFY(sch.libraryExport() == nullptr);
        QVERIFY(sch.librarySettings().isDefault());

        // A file with both (not one saved so): read into the part - which
        // takes the file's when it holds the defaults, and keeps its own
        // otherwise.
        {
            auto* fresh = new LibraryExport();
            fresh->setSchematic(&sch);
            sch.insertComponent(fresh);
            QVERIFY(sch.save() >= 0);
            QString both = read(file);
            both.replace("<Properties>\n", "<Properties>\n  <AlwaysLoadOSDI=1>\n  <LibraryGroundPin=0>\n");
            write(file, both.toUtf8());
            Schematic again(nullptr, file);
            QVERIFY(again.load());
            LibrarySettings fromFile;
            fromFile.alwaysLoadOSDI = true;
            fromFile.groundPin = LibrarySettings::Without;
            QCOMPARE(LibraryExport::settingsOf(again.libraryExport()), fromFile);
            QVERIFY(again.save() >= 0);
            QVERIFY2(!read(file).contains("<AlwaysLoadOSDI"), qPrintable(read(file)));
            both = read(file);
            both.replace("<Properties>\n", "<Properties>\n  <AlwaysModelCards=1>\n");
            write(file, both.toUtf8());
            Schematic third(nullptr, file);
            QVERIFY(third.load());
            QCOMPARE(third.librarySettings(), fromFile);   // (its own: AlwaysModelCards left out)
        }

        // Two (a file so made): Check Schematic says which one is used; and
        // one on a schematic with no ports.
        {
            QString text = read(file);
            const qsizetype at = text.indexOf("  <LibraryExport ");
            QVERIFY(at > 0);
            const QString line = text.mid(at, text.indexOf('\n', at) + 1 - at);
            text.insert(at + line.size(), QString(line).replace(QRegularExpression("<LibraryExport \\w+ "), "<LibraryExport Second "));
            const QString two = write(p + "/two.sch", text.toUtf8());
            Schematic doc(nullptr, two);
            QVERIFY(doc.load());
            int parts = 0;
            for (const Component* c : doc.a_DocComps) parts += LibraryExport::is(c) ? 1 : 0;
            QCOMPARE(parts, 2);
            QStringList said;
            for (const qucs_s::erc::Issue& i : qucs_s::erc::check(&doc, false)) said << i.message;
            QVERIFY2(said.join('\n').contains("Second: a second Library Export - LibExport1 holds the schematic's library settings"),
                     qPrintable(said.join('\n')));
            const QString lone = write(p + "/lone.sch", schematic(resistor("R1", "1k", 100, 100, 0)
                                                                  + "  <LibraryExport LibExport1 1 300 300 -30 20 0 0 \"no\" 1 \"foo\" 1 \"no\" 1 \"yes\" 1>\n"));
            Schematic noPorts(nullptr, lone);
            QVERIFY(noPorts.load());
            QVERIFY(noPorts.libraryGroundPin());
            said.clear();
            for (const qucs_s::erc::Issue& i : qucs_s::erc::check(&noPorts, false)) said << i.message;
            QVERIFY2(said.join('\n').contains("LibExport1: a Library Export on a schematic with no ports"), qPrintable(said.join('\n')));
            // (Its cards, a line that is none: said as the part's.)
            QVERIFY2(said.join('\n').contains("its .model cards (LibExport1, its Library Export) have lines that are no card"),
                     qPrintable(said.join('\n')));
        }
    }

    // A Library Export the schematic's text brings (set_schematic): it takes
    // the Document Settings' library settings, said.
    void aLibraryExportWrittenAsTextHoldsThem()
    {
        Module::registerModules();
        const QString p = project("text", {{"div.sch", divider("1k", "3k")}});
        Schematic sch(nullptr, p + "/div.sch");
        QVERIFY(sch.load());
        LibrarySettings own;
        own.alwaysModelCards = true;
        own.modelCards = ".model m6 good";
        sch.setLibrarySettings(own);
        QString components;
        for (Component* c : sch.a_DocComps) components += c->save() + "\n";
        components += "  <LibraryExport LibExport1 1 300 300 -30 20 0 0 \"no\" 1 \"\" 1 \"no\" 1 \"default\" 1>\n";
        QString error;
        QStringList notes;
        QVERIFY2(sch.replaceContent("<Components>\n" + components + "</Components>\n", &error, &notes), qPrintable(error));
        QVERIFY2(notes.join('\n').contains("LibExport1 took this schematic's Document Settings > Library"), qPrintable(notes.join('\n')));
        QVERIFY(sch.libraryExport() != nullptr);
        QCOMPARE(LibraryExport::settingsOf(sch.libraryExport()), own);
        QCOMPARE(sch.librarySettings(), own);
    }

    // Document Settings > Library, opened at its smallest: each note as
    // tall as it is wrapped - in its tab it was squeezed, its lines drawn
    // over the next control.
    void theLibraryTabIsNotSqueezed()
    {
        Module::registerModules();
        const QString p = project("tab", {{"div.sch", divider("1k", "3k")}});
        Schematic sch(nullptr, p + "/div.sch");
        QVERIFY(sch.load());
        SettingsDialog dialog(&sch);
        auto* tabs = dialog.findChild<QTabWidget*>();
        QVERIFY(tabs != nullptr);
        tabs->setCurrentIndex(tabs->count() - 1);
        QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("Library"));
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        dialog.resize(250, 200);   // (as it opens: grown to its minimum)
        QApplication::processEvents();
        auto* options = dialog.findChild<LibraryOptions*>();
        QVERIFY(options != nullptr);
        int notes = 0;
        for (QLabel* label : options->findChildren<QLabel*>()) {
            if (!label->wordWrap() || !label->isVisible()) continue;
            ++notes;
            QVERIFY2(label->height() >= label->heightForWidth(label->width()),
                     qPrintable(QStringLiteral("%1 high, %2 needed: %3").arg(label->height()).arg(label->heightForWidth(label->width()))
                                    .arg(label->text().left(40))));
        }
        QCOMPARE(notes, 2);
        dialog.close();
    }

    // Its ground pin, the subcircuit's own choice (its Library Export's,
    // or Document Settings'): with gnd or without, whatever the settings or
    // the request say; by default theirs. Create Library says whose.
    void aSubcircuitChoosesItsGroundPin()
    {
        Module::registerModules();
        QVERIFY(!QucsSettings.LibraryGroundPin);
        const QString p = project("pins", {{"plain.sch", divider("1k", "3k")}, {"with.sch", divider("1k", "1k")},
                                           {"without.sch", divider("2k", "1k")}, {"doc.sch", divider("3k", "1k")}});
        const auto choose = [&p](const QString& name, LibrarySettings::GroundPin pin, bool part) {
            Schematic s(nullptr, p + "/" + name);
            QVERIFY(s.load());
            if (part) {
                auto* c = new LibraryExport();
                c->setSchematic(&s);
                s.insertComponent(c);
            }
            LibrarySettings l;
            l.groundPin = pin;
            s.setLibrarySettings(l);
            QVERIFY(s.save() >= 0);
        };
        choose("with.sch", LibrarySettings::With, true);
        choose("without.sch", LibrarySettings::Without, true);
        choose("doc.sch", LibrarySettings::With, false);
        QVERIFY(read(p + "/doc.sch").contains("\n  <LibraryGroundPin=1>\n"));
        const auto pins = [](const QString& library, const QString& sub) {
            const QString line = block(library, sub).section('\n', 0, 0);
            return line.split(' ', Qt::SkipEmptyParts).mid(2);
        };
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "pins";
        const QStringList subs{"plain.sch", "with.sch", "without.sch", "doc.sch"};
        QString log = make(app, "Pins", subs, userLib);
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        QVERIFY2(log.contains("Ground pin: a first pin gnd in its .SUBCKT, as its Library Export LibExport1 asks."), qPrintable(log));
        QVERIFY2(log.contains("Ground pin: none in its .SUBCKT, as its Library Export LibExport1 asks."), qPrintable(log));
        QVERIFY2(log.contains("Ground pin: a first pin gnd in its .SUBCKT, as its Document Settings > Library asks."), qPrintable(log));
        QString lib = read(userLib + "/Pins.lib");
        QCOMPARE(pins(lib, "Pins_plain").size(), 2);
        QCOMPARE(pins(lib, "Pins_with").value(0), QStringLiteral("gnd"));
        QCOMPARE(pins(lib, "Pins_with").size(), 3);
        QCOMPARE(pins(lib, "Pins_without").size(), 2);
        QCOMPARE(pins(lib, "Pins_doc").value(0), QStringLiteral("gnd"));
        // Asked for gnd: the subcircuits that choose keep their choice.
        log = make(app, "PinsGnd", subs, userLib, false, nullptr, true);
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        lib = read(userLib + "/PinsGnd.lib");
        QCOMPARE(pins(lib, "PinsGnd_plain").value(0), QStringLiteral("gnd"));
        QCOMPARE(pins(lib, "PinsGnd_with").value(0), QStringLiteral("gnd"));
        QCOMPARE(pins(lib, "PinsGnd_without").size(), 2);
        QVERIFY2(!pins(lib, "PinsGnd_without").contains("gnd"), qPrintable(pins(lib, "PinsGnd_without").join(' ')));
        QVERIFY(!QucsSettings.LibraryGroundPin);   // (the request's, for that library only)
        // Each part tells which it has.
        QVERIFY(LibComp::takesGround(userLib + "/PinsGnd.lib", "plain", 2));
        QVERIFY(!LibComp::takesGround(userLib + "/PinsGnd.lib", "without", 2));
        QVERIFY(LibComp::takesGround(userLib + "/Pins.lib", "with", 2));
        QVERIFY(!LibComp::takesGround(userLib + "/Pins.lib", "plain", 2));
        // And a circuit placing them: two nodes, or ground and two.
        write(p + "/use.sch", schematic(libPart("X1", "PinsGnd", "without", 300, 100) + libPart("X2", "Pins", "with", 300, 300)));
        const QString net = netlistOf(p + "/use.sch");
        QVERIFY2(lines(net, "^XX1 \\S+ \\S+ PinsGnd_without$") == 1, qPrintable(net));
        QVERIFY2(lines(net, "^XX2 0 \\S+ \\S+ Pins_with$") == 1, qPrintable(net));
        app.ProjName.clear();
        QFile::remove(userLib + "/Pins.lib");
        QFile::remove(userLib + "/PinsGnd.lib");
    }

    // Convert Data File's Qucs library of a SPICE file: refused when the file
    // has no .model card (its subcircuits are not made parts - "Successfully
    // converted", an empty library), the output file left as it was; a
    // model's name keeps its first letter unless a part number follows it.
    void theConverterMakesNoEmptyLibrary()
    {
#ifndef QUCSCONV
        QSKIP("the converter is not built here");
#else
        const QString conv = QStringLiteral(QUCSCONV);
        if (!QFileInfo(conv).isExecutable()) QSKIP("the converter is not built here");
        const QString in = write(dir.filePath("conv/vend.cir"), subckt("VDIV", "3k"));
        const QString out = write(dir.filePath("conv/Out.lib"), "kept");
        QProcess run;
        run.start(conv, {"-if", "spice", "-of", "qucslib", "-ln", "V", "-i", in, "-o", out});
        QVERIFY(run.waitForFinished(30000));
        QVERIFY(run.exitCode() != 0);
        QVERIFY2(QString::fromUtf8(run.readAllStandardError()).contains("its .subckt subcircuits are not converted"), "");
        QCOMPARE(read(out), QStringLiteral("kept"));
        const QString models = write(dir.filePath("conv/m.cir"), "* m\n.model MYNMOS NMOS(LEVEL=1 VTO=0.7)\n.model D1N4148 D(IS=2n)\n.end\n");
        run.start(conv, {"-if", "spice", "-of", "qucslib", "-ln", "M", "-i", models, "-o", dir.filePath("conv/M.lib")});
        QVERIFY(run.waitForFinished(30000));
        QCOMPARE(run.exitCode(), 0);
        const QString lib = read(dir.filePath("conv/M.lib"));
        QVERIFY2(lib.contains("<Component MYNMOS>") && lib.contains("<Component 1N4148>"), qPrintable(lib));
#endif
    }
};

QTEST_MAIN(TestLibraryExport)
#include "test_library_export.moc"
