/*
 * Which OSDI libraries a netlist loads (osdiselection.h): the modules its
 * .model cards name - in the netlist and in the files it includes, and
 * theirs - against what each library of the project defines; one library
 * for a module; a library that cannot be loaded here read for the name;
 * the ngspice netlist of a project loading those (for a simulation and
 * for the DC bias) and no others.
 *
 * With OpenVAF on the machine (QUCS_OPENVAF, openvaf-r or openvaf on
 * PATH) real libraries are compiled and read too.
 */
#include <QtTest>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "schematic.h"
#include "osdiselection.h"
#include "vamodule.h"
#include "erc.h"
#include "components/vacomponent.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The netlist a kernel writes.
class NetlistProbe : public Ngspice
{
public:
    using Ngspice::Ngspice;
    QString netlist()
    {
        QString text;
        QTextStream stream(&text);
        QStringList simulations, vars, outputs;
        createNetlist(stream, simulations, vars, outputs);
        stream.flush();
        return text;
    }
};

void write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

// A file that looks like a library built for another machine: it cannot
// be loaded, its strings are there.
QByteArray foreignLibrary(const QByteArray& strings)
{
    return QByteArray("\xca\xfe\xba\xbe") + " not a library here" + '\0' + strings + '\0';
}

void setBuilt(const QString& path, const QDateTime& when)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(when, QFileDevice::FileModificationTime));
}

// OpenVAF, if it is here: QUCS_OPENVAF, else openvaf-r or openvaf on PATH.
QString openVaf()
{
    const QString given = qEnvironmentVariable("QUCS_OPENVAF");
    if (!given.isEmpty()) return given;
    for (const char* name : {"openvaf-r", "openvaf"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty()) return found;
    }
    return QString();
}

QString resistor(const QString& name)
{
    return QStringLiteral("`include \"disciplines.vams\"\n"
                          "module %1(a, b);\n"
                          "  inout a, b;\n"
                          "  electrical a, b;\n"
                          "  parameter real r = 1e3;\n"
                          "  analog I(a, b) <+ V(a, b) / r;\n"
                          "endmodule\n").arg(name);
}

// A resistor, ground, a .MODEL card of psp103 and an included library that
// uses hicum_l2 and includes another with mextram.
const char* circuit =
    "<Qucs Schematic " PACKAGE_VERSION ">\n"
    "<Properties>\n</Properties>\n"
    "<Symbol>\n</Symbol>\n"
    "<Components>\n"
    "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
    "  <GND * 1 300 200 0 0 0 0>\n"
    "  <SpiceModel SpiceModel1 1 120 300 -27 16 0 0 \".model m1 PSP103 (level=103)\" 1 \"Line_2=\" 0>\n"
    "  <SpiceInclude SpiceInclude1 1 400 300 -27 16 0 0 \"models/devices.lib\" 1 \"\" 0 \"\" 0 \"\" 0 \"\" 0>\n"
    "</Components>\n"
    "<Wires>\n"
    "  <280 100 300 100 \"\" 0 0 0 \"\">\n"
    "  <300 100 300 200 \"\" 0 0 0 \"\">\n"
    "  <220 100 220 200 \"\" 0 0 0 \"\">\n"
    "  <220 200 300 200 \"\" 0 0 0 \"\">\n"
    "</Wires>\n"
    "<Diagrams>\n</Diagrams>\n"
    "<Paintings>\n</Paintings>\n";

QStringList preOsdi(const QString& netlist)
{
    QStringList files;
    static const QRegularExpression line(QStringLiteral("^pre_osdi '([^']*)'$"),
                                         QRegularExpression::MultilineOption);
    for (auto it = line.globalMatch(netlist); it.hasNext();)
        files << QFileInfo(it.next().captured(1)).canonicalFilePath();
    return files;
}

} // namespace

class TestOsdiSelection : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString workspace, project;

    QString real(const QString& path) const { return QFileInfo(path).canonicalFilePath(); }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.S4Qworkdir = dir.filePath("cache-workdir");
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();

        workspace = dir.filePath("workspace");
        project = workspace + "/osdi_prj";
        QVERIFY(QDir().mkpath(project));
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
        QucsSettings.projsDir.setPath(workspace);
        QucsSettings.QucsWorkDir.setPath(workspace);
    }

    // ---- what a netlist uses ----------------------------------------

    void theTypesOfTheModelCards()
    {
        const QString spice =
            ".model n1 BSIMCMG (l=1u)\n"
            ".MODEL q2 npn(bf=100)\r\n"
            "* .model hidden psp103\n"
            ".model c1\n"
            "+ capmod (c=1p)\n"
            "   .model  x   hicum_l2\n"
            ".subckt s a b\n"
            ".model inner mextram\n"
            ".ends\n"
            "R1 a b 1k\n"
            "Nmos d g s b n1 l=1u\n"
            ".modelx not a card\n";
        QCOMPARE(osdi::modelTypes(spice),
                 (QSet<QString>{"bsimcmg", "npn", "capmod", "hicum_l2", "mextram"}));
    }

    void theFilesANetlistIncludes()
    {
        const QString base = dir.filePath("inc");
        const QString spice =
            ".include \"a b/lib.cir\"\n"
            ".INC 'q.lib'\n"
            ".lib /abs/pdk.lib tt\n"
            ".include rel/x.mod\n"
            ".include rel/x.mod\n"
            "* .include hidden.lib\n";
        QCOMPARE(osdi::includedFiles(spice, base),
                 (QStringList{base + "/a b/lib.cir", base + "/q.lib", QDir::cleanPath("/abs/pdk.lib"),
                              base + "/rel/x.mod"}));
    }

    void theCardsOfIncludedFilesCount()
    {
        const QString base = dir.filePath("libs");
        write(base + "/one.lib", ".model m1 psp103\n.include \"sub/two.lib\"\n.lib tt\n.endl\n");
        write(base + "/sub/two.lib", ".model m2 hicum_l2 (is=1e-16)\n.include \"../one.lib\"\n");
        const QSet<QString> types = osdi::usedModelTypes(".model r0 r\n.lib \"one.lib\" tt\n", base);
        QCOMPARE(types, (QSet<QString>{"r", "psp103", "hicum_l2"}));   // and the loop ends

        // A changed file is read again.
        write(base + "/sub/two.lib", ".model m2 mextram\n");
        setBuilt(base + "/sub/two.lib", QDateTime::currentDateTime().addSecs(5));
        QCOMPARE(osdi::usedModelTypes(".include one.lib\n", base),
                 (QSet<QString>{"psp103", "mextram"}));
    }

    // ---- what the libraries define ----------------------------------

    void aLibraryThatCannotBeLoadedIsReadForTheName()
    {
        const QString file = dir.filePath("foreign/psp.osdi");
        write(file, foreignLibrary(QByteArray("PSP103") + '\0' + "osdi_log"));
        bool readable = true;
        QVERIFY(osdi::modulesOf(file, &readable).isEmpty());
        QVERIFY(!readable);
        QVERIFY(osdi::defines(file, "psp103"));
        QVERIFY(osdi::defines(file, "Psp103"));
        QVERIFY(!osdi::defines(file, "psp"));        // a string of its own, not a part of one
        QVERIFY(!osdi::defines(file, "hicum_l2"));

        const QString part = dir.filePath("foreign/part.osdi");
        write(part, foreignLibrary("xpsp103"));
        QVERIFY(!osdi::defines(part, "psp103"));
    }

    void oneLibraryForEachModuleUsed()
    {
        const QString libs = dir.filePath("choice");
        const QString psp = libs + "/psp103.osdi";
        const QString oldPsp = libs + "/old/psp103.osdi";
        const QString hicum = libs + "/hicum.osdi";
        const QString both = libs + "/both.osdi";
        const QString unused = libs + "/unused.osdi";
        write(psp, foreignLibrary("psp103"));
        write(oldPsp, foreignLibrary("psp103"));
        write(hicum, foreignLibrary("hicum_l2"));
        write(both, foreignLibrary(QByteArray("hicum_l2") + '\0' + "mextram"));
        write(unused, foreignLibrary("bsimcmg"));
        const QDateTime now = QDateTime::currentDateTime();
        setBuilt(oldPsp, now.addDays(-3));
        setBuilt(psp, now.addDays(-1));
        setBuilt(hicum, now.addDays(-2));
        setBuilt(both, now.addDays(-4));

        const QStringList all{oldPsp, psp, hicum, both, unused};
        QStringList notes;
        // hicum_l2 is in two: the newer. mextram: only in both.osdi, which
        // then has hicum_l2 as well - taken once, hicum.osdi stays.
        QCOMPARE(osdi::needed(all, {"psp103", "hicum_l2", "npn", "r"}, &notes), (QStringList{psp, hicum}));
        QCOMPARE(notes.size(), 2);
        QVERIFY(notes.join('\n').contains("psp103 from " + QDir::toNativeSeparators(psp)));
        QVERIFY(notes.join('\n').contains(QDir::toNativeSeparators(oldPsp)));

        QCOMPARE(osdi::needed(all, {"mextram"}), (QStringList{both}));
        QCOMPARE(osdi::needed(all, {"hicum_l2", "mextram"}), (QStringList{both}));   // one library does both
        QCOMPARE(osdi::needed(all, {"npn", "d"}), QStringList());
        QCOMPARE(osdi::needed(all, {}), QStringList());
    }

    void realLibrariesAreReadExactly()
    {
        const QString compiler = openVaf();
        if (compiler.isEmpty())
            QSKIP("OpenVAF is not installed (QUCS_OPENVAF, openvaf-r, openvaf)");
        const QString base = dir.filePath("compiled");
        write(base + "/resa.va", resistor("ResA").toUtf8());
        write(base + "/pair.va", (resistor("resb") + resistor("resc").section('\n', 1)).toUtf8());
        for (const QString& name : {QStringLiteral("resa"), QStringLiteral("pair")}) {
            QProcess p;
            p.setProcessChannelMode(QProcess::MergedChannels);
            p.start(compiler, {base + "/" + name + ".va", "-o", base + "/" + name + ".osdi"});
            QVERIFY(p.waitForFinished(120000));
            QVERIFY2(p.exitCode() == 0, p.readAll().constData());
        }
        bool readable = false;
        QCOMPARE(osdi::modulesOf(base + "/resa.osdi", &readable), QStringList{"resa"});
        QVERIFY(readable);
        QCOMPARE(osdi::modulesOf(base + "/pair.osdi"), (QStringList{"resb", "resc"}));
        QVERIFY(osdi::defines(base + "/resa.osdi", "RESA"));
        QVERIFY(!osdi::defines(base + "/resa.osdi", "a"));   // "a", a port: a string in it, not a module
        const QStringList libs{base + "/resa.osdi", base + "/pair.osdi"};
        QCOMPARE(osdi::needed(libs, {"resc"}), QStringList{base + "/pair.osdi"});
        QCOMPARE(osdi::needed(libs, {"resa", "r"}), QStringList{base + "/resa.osdi"});
        // Built here: not for another platform - by this platform, by a
        // program of it (this test), by the ngspice there is.
        QVERIFY(!osdi::builtForAnotherPlatform(base + "/resa.osdi"));
        QVERIFY(!osdi::builtForAnotherPlatform(base + "/resa.osdi", QCoreApplication::applicationFilePath()));
        const QString ngspice = QFileInfo(QStandardPaths::findExecutable("ngspice")).canonicalFilePath();
        if (!ngspice.isEmpty())
            QVERIFY2(!osdi::builtForAnotherPlatform(base + "/resa.osdi", ngspice), qPrintable(ngspice));
    }

    // ---- the netlist -------------------------------------------------

    void theNetlistLoadsTheLibrariesItUses()
    {
        write(project + "/circuit.sch", circuit);
        write(project + "/models/devices.lib", ".model q1 hicum_l2 (is=1e-16)\n.include \"more.lib\"\n");
        write(project + "/models/more.lib", "* more\n.model q2 MEXTRAM\n");
        write(project + "/psp103.osdi", foreignLibrary("psp103"));
        write(project + "/old/psp103.osdi", foreignLibrary("psp103"));
        write(project + "/models/hicum.osdi", foreignLibrary("hicum_l2"));
        write(project + "/lib/mextram.osdi", foreignLibrary("MEXTRAM"));
        write(project + "/unused.osdi", foreignLibrary("bsimcmg"));
        write(project + "/sub/also_unused.osdi", foreignLibrary("xpsp103"));
        const QDateTime now = QDateTime::currentDateTime();
        setBuilt(project + "/old/psp103.osdi", now.addDays(-2));
        setBuilt(project + "/psp103.osdi", now.addDays(-1));

        QucsApp app(false);
        MainGuard guard(&app);
        app.openProject(project);
        QCOMPARE(app.ProjName, QString("osdi"));

        Schematic sch(nullptr, project + "/circuit.sch");
        QVERIFY(sch.load());
        const QStringList expected{real(project + "/psp103.osdi"), real(project + "/lib/mextram.osdi"),
                                   real(project + "/models/hicum.osdi")};
        {
            NetlistProbe kernel(&sch);
            const QString netlist = kernel.netlist();
            QStringList loaded = preOsdi(netlist);
            loaded.sort();
            QStringList sorted = expected;
            sorted.sort();
            QVERIFY2(loaded == sorted, qPrintable(netlist));
            QVERIFY(netlist.contains("* OSDI: psp103 from "));
            // Loaded in the .control section, before the circuit is read.
            QVERIFY(netlist.indexOf("pre_osdi") > netlist.indexOf(".control"));
        }
        {
            // The DC bias too (its netlist had no OSDI library at all).
            NetlistProbe kernel(&sch);
            kernel.setOperatingPointOnly(true);
            const QString netlist = kernel.netlist();
            QStringList loaded = preOsdi(netlist);
            loaded.sort();
            QStringList sorted = expected;
            sorted.sort();
            QVERIFY2(loaded == sorted, qPrintable(netlist));
            QVERIFY(netlist.indexOf("pre_osdi") < netlist.indexOf("\nop\n"));
        }
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
    }

    // ---- compiling before a simulation -------------------------------

    void theModulesOfASource()
    {
        const QString source =
            "// module commented(a);\n"
            "`include \"disciplines.vams\"\n"
            "module First (a, b); endmodule\n"
            "/* module hidden; */\n"
            "module second; endmodule\n"
            "macromodule third #(parameter p = 1) (x); endmodule\n";
        QCOMPARE(vamodule::sourceModules(source), (QStringList{"First", "second", "third"}));
    }

    void whatIsCompiled()
    {
        const QString base = dir.filePath("builds");
        const QDateTime now = QDateTime::currentDateTime();
        write(base + "/amp.va", "module amp(a); endmodule\n");              // used, never built
        write(base + "/res.va", "module res(a); endmodule\n");              // used, built after it
        write(base + "/res.osdi", foreignLibrary("res"));
        setBuilt(base + "/res.va", now.addSecs(-60));
        write(base + "/top.va", "`include \"common.vams\"\n`include \"disciplines.vams\"\nmodule top(a); endmodule\n");
        write(base + "/common.vams", "`include \"deeper/more.vams\"\n");
        write(base + "/deeper/more.vams", "// constants\n");
        write(base + "/top.osdi", foreignLibrary("top"));
        setBuilt(base + "/top.va", now.addSecs(-60));
        setBuilt(base + "/common.vams", now.addSecs(-60));
        setBuilt(base + "/top.osdi", now.addSecs(-30));
        setBuilt(base + "/deeper/more.vams", now.addSecs(-10));              // changed since top.osdi
        write(base + "/other.va", "module other(a); endmodule\n");          // used, a library elsewhere has it
        write(base + "/lib/other.osdi", foreignLibrary("other"));
        write(base + "/pair.va", "module p1(a); endmodule\nmodule P2(a); endmodule\n");
        write(base + "/unused.va", "module unused(a); endmodule\n");

        QCOMPARE(osdi::sourceIncludes(base + "/top.va"),
                 (QStringList{base + "/common.vams", base + "/deeper/more.vams"}));
        QVERIFY(osdi::sourceDefines(base + "/pair.va", "p2"));
        QVERIFY(!osdi::sourceDefines(base + "/pair.va", "p3"));

        const QStringList sources{base + "/amp.va", base + "/res.va", base + "/top.va", base + "/other.va",
                                  base + "/pair.va", base + "/unused.va"};
        const QStringList libraries{base + "/res.osdi", base + "/top.osdi", base + "/lib/other.osdi"};
        const QList<osdi::Build> list =
            osdi::builds(sources, libraries, {"amp", "res", "top", "other", "p2", "npn"});
        QStringList built;
        for (const osdi::Build& b : list) built << QFileInfo(b.source).fileName();
        QCOMPARE(built, (QStringList{"amp.va", "top.va", "pair.va"}));
        QVERIFY(list[0].missing);
        QCOMPARE(list[0].library, base + "/amp.osdi");
        QVERIFY(!list[1].missing);
        QCOMPARE(list[2].modules, QStringList{"p2"});

        // The source changed after its library: built again.
        setBuilt(base + "/res.va", now.addSecs(10));
        QCOMPARE(osdi::builds(sources, libraries, {"res"}).size(), 1);
    }

    void aSimulationCompilesWhatItUsesFirst()
    {
        const QString calls = dir.filePath("openvaf-calls.log");
        const QString record = dir.filePath("ngspice-got.cir");
        const QString openvaf = dir.filePath("fake-openvaf.sh");
        write(openvaf, QStringLiteral(
            "#!/bin/sh\n"
            "echo \"$1\" >> \"%1\"\n"
            "if grep -q broken \"$1\"; then echo \"error: cannot parse $1\"; exit 1; fi\n"
            "echo \"compiled $1\"\n"
            "printf '\\000%s\\000' \"$(basename \"${1%.va}\")\" > \"${1%.va}.osdi\"\n").arg(calls).toUtf8());
        const QString ngspice = dir.filePath("fake-ngspice.sh");
        write(ngspice, QStringLiteral(
            "#!/bin/sh\n"
            "for a in \"$@\"; do case \"$a\" in *.cir) cp \"$a\" \"%1\";; esac; done\n"
            "echo \"fake ngspice\"\n").arg(record).toUtf8());
        for (const QString& script : {openvaf, ngspice})
            QVERIFY(QFile::setPermissions(script, QFile::permissions(script) | QFile::ExeOwner));

        const QString vaProject = workspace + "/va_prj";
        write(vaProject + "/amp.va", "module amp(a, b); endmodule\n");
        write(vaProject + "/models/unused.va", "module unused(a, b); endmodule\n");
        write(vaProject + "/broken.va", "module broken(a, b); // broken\nendmodule\n");
        QByteArray uses(circuit);
        uses.replace("\"models/devices.lib\"", "\"\"");
        uses.replace(".model m1 PSP103 (level=103)", ".model m1 amp");
        uses.replace("</Components>", "  <.DC DC1 1 500 200 0 40 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 "
                                      "\"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0 \"no\" 0>\n</Components>");
        write(vaProject + "/uses.sch", uses);
        QByteArray fails(uses);
        fails.replace(".model m1 amp", ".model m1 broken");
        write(vaProject + "/fails.sch", fails);

        const QString savedNgspice = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QucsSettings.OpenVAFExecutable = openvaf;
        QucsApp app(false);
        MainGuard guard(&app);
        app.openProject(vaProject);
        QCOMPARE(app.ProjName, QString("va"));

        QPlainTextEdit console;
        QListWidget log;
        QProgressBar progress;
        const auto simulate = [&](const QString& file, bool* error) {
            Schematic sch(nullptr, file);
            if (!sch.load()) return false;
            SimulationRun run(&sch, false);
            run.attach(&console, &log, &progress);
            QSignalSpy done(&run, &SimulationRun::simulated);
            run.start();
            const bool finished = done.size() > 0 || done.wait(30000);
            *error = run.hasError();
            return finished;
        };
        const auto lines = [&](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts)
                                               : QStringList();
        };
        bool error = false;

        // amp.va is compiled - not the others - and the netlist loads it.
        QVERIFY(simulate(vaProject + "/uses.sch", &error));
        QCOMPARE(lines(calls), QStringList{vaProject + "/amp.va"});
        QVERIFY(QFileInfo::exists(vaProject + "/amp.osdi"));
        QVERIFY2(lines(record).contains("pre_osdi '" + vaProject + "/amp.osdi'"), qPrintable(lines(record).join('\n')));
        QVERIFY2(console.toPlainText().contains("compiled " + vaProject + "/amp.va"), qPrintable(console.toPlainText()));
        QVERIFY(console.toPlainText().contains("fake ngspice"));   // the compilation stays above

        // Built and unchanged: nothing to compile.
        QVERIFY(simulate(vaProject + "/uses.sch", &error));
        QCOMPARE(lines(calls).size(), 1);

        // Changed: compiled again.
        setBuilt(vaProject + "/amp.va", QDateTime::currentDateTime().addSecs(30));
        QVERIFY(simulate(vaProject + "/uses.sch", &error));
        QCOMPARE(lines(calls).size(), 2);

        // A source that does not compile: no simulation, the error said.
        QFile::remove(record);
        QVERIFY(simulate(vaProject + "/fails.sch", &error));
        QVERIFY(error);
        QVERIFY(!QFileInfo::exists(record));
        QCOMPARE(lines(calls).last(), vaProject + "/broken.va");
        bool said = false;
        for (int i = 0; i < log.count(); ++i)
            said = said || log.item(i)->text().contains("OpenVAF could not compile");
        QVERIFY(said);

        // No OpenVAF: simulated with what there is, the stale library said.
        QucsSettings.OpenVAFExecutable.clear();
        setBuilt(vaProject + "/amp.va", QDateTime::currentDateTime().addSecs(60));
        log.clear();
        QVERIFY(simulate(vaProject + "/uses.sch", &error));
        QCOMPARE(lines(calls).size(), 3);   // not compiled
        QVERIFY(QFileInfo::exists(record));
        said = false;
        for (int i = 0; i < log.count(); ++i)
            said = said || log.item(i)->text().contains("is older than");
        QVERIFY(said);

        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
        QucsSettings.NgspiceExecutable = savedNgspice;
    }

    void theCheckFindsAModuleNowhereInTheProject()
    {
        Module::registerModules();   // a QucsApp's destructor unregisters them
        const QString vaProject = workspace + "/erc_prj";
        write(vaProject + "/circuit.sch", circuit);
        QucsApp app(false);
        MainGuard guard(&app);
        app.openProject(vaProject);
        Schematic sch(nullptr, vaProject + "/circuit.sch");
        QVERIFY(sch.load());
        auto* ghost = new vacomponent(vamodule::propsObject(vamodule::readSource("module ghost(a, b); endmodule")));
        ghost->Name = "X1";
        sch.a_DocComps.push_back(ghost);
        const auto warned = [&] {
            for (const erc::Issue& issue : erc::check(&sch))
                if (issue.component == "X1" && issue.message.contains("Verilog-A module ghost")) return true;
            return false;
        };
        QVERIFY(warned());
        write(vaProject + "/ghost.va", "module ghost(a, b); endmodule\n");   // compiled before the simulation
        QVERIFY(!warned());
        QFile::remove(vaProject + "/ghost.va");
        write(vaProject + "/lib/ghost.osdi", foreignLibrary("ghost"));
        QVERIFY(!warned());
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
    }

    void withoutAProjectNoLibraryIsLoaded()
    {
        Module::registerModules();   // a QucsApp's destructor unregisters them
        Schematic sch(nullptr, project + "/circuit.sch");
        QVERIFY(sch.load());
        NetlistProbe kernel(&sch);
        QVERIFY(!kernel.netlist().contains("pre_osdi"));   // headless: no project
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestOsdiSelection test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_osdi_selection.moc"
