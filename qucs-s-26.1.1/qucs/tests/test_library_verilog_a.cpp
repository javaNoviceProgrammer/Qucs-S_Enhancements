/*
 * Libraries that bring their Verilog-A: Project > Create Library copies the
 * .va sources the subcircuits use - and the files they include - into the
 * library's folder when the setting says so, never a compiled .osdi model,
 * which runs on one platform only; a circuit that uses the library, in any
 * project or none, has the source compiled beside it before a simulation
 * and loads the model, and a model built on another platform is compiled
 * again instead of being handed to ngspice. Stand-in .osdi files carry the
 * module's name as a string, as a library holds it.
 */
#include <QtTest>
#include <QCheckBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "osdiselection.h"
#include "projectlibraries.h"
#include "schematic.h"
#include "settings.h"
#include "components/libcomp.h"
#include "dialogs/librarydialog.h"
#include "dialogs/qucssettingsdialog.h"
#include "dialogs/settingsdialog.h"
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

QByteArray bytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void setBuilt(const QString& path, const QDateTime& when)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(when, QFileDevice::FileModificationTime));
}

constexpr QFileDevice::Permissions kReadOnlyFolder =
    QFileDevice::ReadOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup
    | QFileDevice::ReadOther | QFileDevice::ExeOther;
constexpr QFileDevice::Permissions kFolder = kReadOnlyFolder | QFileDevice::WriteOwner;

// A schematic with one part of the library \a lib (its Lib: a path, or a
// name), and two grounds unless not \a grounded: whether they touch its pins
// is the symbol's, which a library made here sizes by the font's measure.
QByteArray usesLibrary(const QString& lib, bool grounded = true)
{
    return "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
           "  <Lib X1 1 100 100 20 -20 0 0 \"" + lib.toUtf8() + "\" 0 \"sub\" 0>\n"
           + QByteArray(grounded ? "  <GND * 1 70 100 0 0 0 0>\n  <GND * 1 130 100 0 0 0 0>\n" : "")
           + "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
}

// A stand-in library: the name between two NULs, after \a header.
QByteArray osdi(const QByteArray& module, const QByteArray& header = QByteArray())
{
    return header + QByteArray(64, '\0') + QByteArray(1, '\0') + module + QByteArray(1, '\0');
}

// The first bytes of a shared library for a platform and processor.
QByteArray elf(quint16 machine)
{
    QByteArray h(64, '\0');
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
    h[4] = 2;   // 64-bit
    h[5] = 1;   // little-endian
    h[18] = char(machine & 0xff);
    h[19] = char(machine >> 8);
    return h;
}

QByteArray machO(quint32 cpu)
{
    QByteArray h(64, '\0');
    const quint32 magic = 0xfeedfacfu;
    for (int i = 0; i < 4; ++i) {
        h[i] = char((magic >> (8 * i)) & 0xff);
        h[4 + i] = char((cpu >> (8 * i)) & 0xff);
    }
    return h;
}

QByteArray pe(quint16 machine)
{
    QByteArray h(256, '\0');
    h[0] = 'M'; h[1] = 'Z';
    h[0x3c] = char(0x80);
    h[0x80] = 'P'; h[0x81] = 'E';
    h[0x84] = char(machine & 0xff);
    h[0x85] = char(machine >> 8);
    return h;
}

constexpr quint16 kElfX86 = 62, kElfArm = 183, kPeX86 = 0x8664, kPeArm = 0xaa64;
constexpr quint32 kMachX86 = 0x01000007u, kMachArm = 0x0100000cu;

// A header this platform loads, and one it does not.
QByteArray nativeHeader()
{
#if defined(Q_OS_MACOS)
#  if defined(Q_PROCESSOR_ARM_64)
    return machO(kMachArm);
#  else
    return machO(kMachX86);
#  endif
#elif defined(Q_OS_WIN)
#  if defined(Q_PROCESSOR_ARM_64)
    return pe(kPeArm);
#  else
    return pe(kPeX86);
#  endif
#else
#  if defined(Q_PROCESSOR_ARM_64)
    return elf(kElfArm);
#  else
    return elf(kElfX86);
#  endif
#endif
}

QByteArray foreignHeader()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    return elf(kElfX86);
#else
    return machO(kMachArm);
#endif
}
} // namespace

class TestLibraryVerilogA : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString workspace, project, userLib, team;

    // Project > Create Library with the subcircuit \a schematic, no
    // descriptions; the dialog's messages.
    QString createLibrary(QucsApp& app, const QString& name, const QString& schematic = "sub.sch")
    {
        LibraryDialog dialog(&app);
        dialog.fillSchematicList({schematic});
        auto* nameEdit = dialog.findChild<QLineEdit*>();
        if (nameEdit == nullptr) return {};
        nameEdit->setText(name);
        for (QCheckBox* box : dialog.findChildren<QCheckBox*>())
            if (box->text() == "Add subcircuit description") box->setChecked(false);
        QMetaObject::invokeMethod(&dialog, "slotCreateNext");
        auto* messages = dialog.findChild<QPlainTextEdit*>();
        return messages != nullptr ? messages->toPlainText() : QString();
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

    // A library made of the project's \a subcircuits into \a folder (the
    // request's \a ground pin); the messages.
    QString makeLibrary(QucsApp& app, const QString& name, const QStringList& subcircuits, const QString& folder,
                        bool ground = false)
    {
        LibraryDialog dialog(&app);
        dialog.fillSchematicList(subcircuits);
        LibraryDialog::Request request;
        request.name = name;
        request.subcircuits = subcircuits;
        request.folder = folder;
        request.embedVerilogA = QucsSettings.EmbedVerilogAInLibraries;
        request.groundPin = ground;
        QString log, error;
        if (!dialog.create(request, &log, &error)) return QStringLiteral("not made: ") + error + "\n" + log;
        return log;
    }

    // The nodes of the part X1 in the netlist \a netlist: what is between its
    // name and its subcircuit's, \a type.
    static QStringList nodesOfX1(const QString& netlist, const QString& type)
    {
        for (const QString& line : netlist.split('\n')) {
            const QStringList words = line.split(' ', Qt::SkipEmptyParts);
            if (words.isEmpty() || words.first() != "XX1") continue;
            const qsizetype at = words.indexOf(type);
            return at > 0 ? words.mid(1, at - 1) : QStringList{"(no " + type + ")"};
        }
        return {"(no XX1)"};
    }

    // The pins of the .SUBCKT line in \a library.
    static QStringList subcircuitPins(const QString& library)
    {
        for (const QString& line : library.split('\n'))
            if (line.startsWith(".SUBCKT ")) return line.split(' ', Qt::SkipEmptyParts).mid(2);
        return {"(no .SUBCKT)"};
    }

    // What a simulation of \a file compiles first.
    QList<qucs_s::osdi::Build> buildsFor(const QString& file)
    {
        Schematic sch(nullptr, file);
        if (!sch.load()) return {};
        Ngspice kernel(&sch);
        kernel.setWorkdir(dir.filePath("kernel"));
        return kernel.verilogABuilds();
    }

    // Where the cache keeps a library compiled for a source.
    QString inCache(const QString& name) const
    {
        return QDir(misc::cacheDir()).absoluteFilePath("osdi/" + name + "-");
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QDir().mkpath(dir.filePath("kernel"));
        QucsSettings.S4Qworkdir = dir.filePath("kernel");

        workspace = dir.filePath("workspace");
        project = workspace + "/vaproj_prj";
        userLib = workspace + "/user_lib";
        QVERIFY(QDir().mkpath(project));
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
        QucsSettings.QucsWorkDir.setPath(project);

        // good.va (with a file it includes) and its model; other.va, not
        // used; a model with no source.
        write(project + "/good.va", "`include \"disciplines.vams\"\n`include \"inc/common.vams\"\n"
                                    "module good(p, n);\nendmodule\n");
        write(project + "/inc/common.vams", "// shared\n");
        write(project + "/good.osdi", osdi("good"));
        write(project + "/other.va", "module unused(p, n);\nendmodule\n");
        write(project + "/other.osdi", osdi("unused"));
        write(project + "/binonly.osdi", osdi("binonly"));
        // A two-port subcircuit whose model card is of the module good.
        write(project + "/sub.sch",
              "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
              "  <Port P1 1 220 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
              "  <Port P2 1 280 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n"
              "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
              "  <SpiceModel SpiceModel1 1 120 300 -27 16 0 0 \".model m1 good\" 1 \"\" 0>\n"
              "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        // One whose model card is of the module with no source.
        write(project + "/bin.sch",
              QString(read(project + "/sub.sch")).replace(".model m1 good", ".model m2 binonly").toUtf8());
    }

    // The format and the processor of a library, against those of this
    // Qucs-S; a file of no known format is not said to be foreign.
    void librariesOfAnotherPlatformAreRecognised()
    {
        using qucs_s::osdi::builtForAnotherPlatform;
        const QString native = write(dir.filePath("h/native.osdi"), osdi("x", nativeHeader()));
        const QString foreign = write(dir.filePath("h/foreign.osdi"), osdi("x", foreignHeader()));
        const QString plain = write(dir.filePath("h/plain.osdi"), osdi("x"));
        QVERIFY(!builtForAnotherPlatform(native));
        QVERIFY(builtForAnotherPlatform(foreign));   // another format
        QVERIFY(!builtForAnotherPlatform(plain));    // no format to tell by
        QVERIFY(!builtForAnotherPlatform(dir.filePath("h/missing.osdi")));
        // A universal magic followed by no count of parts: not a header.
        QVERIFY(!builtForAnotherPlatform(write(dir.filePath("h/cafe.osdi"), "\xca\xfe\xba\xbe not a library")));

        // The processors: of the simulator that loads the library.
        const QString armSim = write(dir.filePath("h/arm-ngspice"), elf(kElfArm));
        const QString x86Sim = write(dir.filePath("h/x86-ngspice"), elf(kElfX86));
        const QString armLib = write(dir.filePath("h/arm.osdi"), osdi("x", elf(kElfArm)));
        const QString x86Lib = write(dir.filePath("h/x86.osdi"), osdi("x", elf(kElfX86)));
        QVERIFY(!builtForAnotherPlatform(armLib, armSim));
        QVERIFY(builtForAnotherPlatform(armLib, x86Sim));
        QVERIFY(builtForAnotherPlatform(x86Lib, armSim));
        QVERIFY(builtForAnotherPlatform(write(dir.filePath("h/mach.osdi"), osdi("x", machO(kMachArm))), armSim));
        // A universal simulator loads either.
        QByteArray fat(64, '\0');
        const char head[] = {'\xca', '\xfe', '\xba', '\xbe', 0, 0, 0, 2};
        fat.replace(0, 8, QByteArray(head, 8));
        for (int i = 0; i < 2; ++i) {
            const quint32 cpu = i == 0 ? kMachX86 : kMachArm;
            for (int b = 0; b < 4; ++b) fat[8 + 20 * i + b] = char((cpu >> (8 * (3 - b))) & 0xff);
        }
        const QString universal = write(dir.filePath("h/universal-ngspice"), fat);
        QVERIFY(!builtForAnotherPlatform(write(dir.filePath("h/m-arm.osdi"), osdi("x", machO(kMachArm))), universal));
        QVERIFY(!builtForAnotherPlatform(write(dir.filePath("h/m-x86.osdi"), osdi("x", machO(kMachX86))), universal));
        // A simulator that is a script: this Qucs-S's platform stands for it.
        QVERIFY(builtForAnotherPlatform(foreign, write(dir.filePath("h/script-ngspice"), "#!/bin/sh\n")));
        QVERIFY(!builtForAnotherPlatform(native, dir.filePath("h/script-ngspice")));
    }

    // On: the source of the module the subcircuit uses, and what it
    // includes, go into the library; nothing else does - no compiled model,
    // and one a library made before had compiled is gone.
    void theLibraryEmbedsTheVerilogAItUses()
    {
        QucsSettings.EmbedVerilogAInLibraries = true;
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "vaproj";
        QucsSettings.QucsWorkDir.setPath(project);
        write(userLib + "/VaLib/good.osdi", osdi("good"));   // compiled from the library made before

        const QString log = createLibrary(app, "VaLib");
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        QVERIFY2(log.contains("Embedding Verilog-A: good.va, inc/common.vams (compiled with OpenVAF where "
                              "the library is used)"), qPrintable(log));
        QVERIFY2(!log.contains("Warning"), qPrintable(log));
        const QString lib = read(userLib + "/VaLib.lib");
        QVERIFY2(lib.contains("<SpiceAttach \"good.va\">"), qPrintable(lib));
        QVERIFY2(!lib.contains(".osdi"), qPrintable(lib));
        QVERIFY(QFileInfo::exists(userLib + "/VaLib/good.va"));
        QVERIFY(QFileInfo::exists(userLib + "/VaLib/inc/common.vams"));
        QVERIFY(!QFileInfo::exists(userLib + "/VaLib/good.osdi"));
        QVERIFY(!QFileInfo::exists(userLib + "/VaLib/other.va"));
        QVERIFY(!QFileInfo::exists(userLib + "/VaLib/other.osdi"));
        QCOMPARE(read(userLib + "/VaLib/good.va"), read(project + "/good.va"));
    }

    // A module the project has only compiled: not embedded, and said.
    void aModelWithNoSourceIsNotEmbedded()
    {
        QucsSettings.EmbedVerilogAInLibraries = true;
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "vaproj";
        QucsSettings.QucsWorkDir.setPath(project);

        const QString log = createLibrary(app, "BinLib", "bin.sch");
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        QVERIFY2(log.contains("Warning: no Verilog-A source of binonly, only a compiled model (.osdi)"),
                 qPrintable(log));
        QVERIFY(!log.contains("Embedding Verilog-A"));
        const QString lib = read(userLib + "/BinLib.lib");
        QVERIFY2(!lib.contains("binonly.osdi"), qPrintable(lib));
        QVERIFY(!QFileInfo::exists(userLib + "/BinLib/binonly.osdi"));
    }

    // Off: the subcircuits and their symbols only, as before.
    void offTheLibraryHoldsTheSubcircuitsOnly()
    {
        QucsSettings.EmbedVerilogAInLibraries = false;
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "vaproj";
        QucsSettings.QucsWorkDir.setPath(project);

        const QString log = createLibrary(app, "PlainLib");
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        QVERIFY(!log.contains("Embedding Verilog-A"));
        const QString lib = read(userLib + "/PlainLib.lib");
        QVERIFY(lib.contains("<Component sub>"));
        QVERIFY2(!lib.contains("good.osdi") && !lib.contains("good.va"), qPrintable(lib));
        QVERIFY(!QFileInfo::exists(userLib + "/PlainLib/good.osdi"));
        QucsSettings.EmbedVerilogAInLibraries = true;
    }

    // A circuit elsewhere - no project open - that uses the library has
    // the embedded source compiled beside it before the first simulation,
    // then loads the model; one built on another platform is not handed to
    // ngspice but compiled again.
    void aCircuitUsingTheLibraryCompilesAndLoadsItsModel()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName.clear();
        const QString use = write(dir.filePath("elsewhere/use.sch"), usesLibrary(userLib + "/VaLib"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("elsewhere"));
        // The library found through its canonical path (/private/var for
        // /var on macOS).
        const QString model = QFileInfo(userLib + "/VaLib").canonicalFilePath() + "/good.osdi";

        // Not compiled yet: compiled, beside the source, before it runs.
        QString netlist = netlistOf(use);
        QVERIFY2(!netlist.contains("pre_osdi"), qPrintable(netlist));
        QVERIFY2(netlist.contains(".model m1 good"), qPrintable(netlist));
        QList<qucs_s::osdi::Build> builds = buildsFor(use);
        QCOMPARE(builds.size(), 1);
        QCOMPARE(QFileInfo(builds.first().source).fileName(), QString("good.va"));
        QCOMPARE(builds.first().library, model);
        QVERIFY(builds.first().into == qucs_s::osdi::Into::Beside);
        QVERIFY(builds.first().missing);
        QVERIFY(!builds.first().foreign);

        // Compiled (as OpenVAF would): loaded, and not compiled again.
        write(model, osdi("good", nativeHeader()));
        netlist = netlistOf(use);
        QVERIFY2(netlist.contains("pre_osdi '" + model + "'"), qPrintable(netlist));
        QVERIFY(buildsFor(use).isEmpty());   // the model is newer than its source

        // The model as it came from another platform: not handed to
        // ngspice, and kept for the computers it was built for - this
        // platform's is compiled into the cache and loaded from there.
        write(model, osdi("good", foreignHeader()));
        netlist = netlistOf(use);
        QVERIFY2(!netlist.contains("pre_osdi"), qPrintable(netlist));
        QVERIFY2(netlist.contains("* OSDI: ") && netlist.contains("built for another platform"), qPrintable(netlist));
        builds = buildsFor(use);
        QCOMPARE(builds.size(), 1);
        QVERIFY(builds.first().foreign);
        QVERIFY(!builds.first().missing);
        QCOMPARE(builds.first().built, model);
        QVERIFY(builds.first().into == qucs_s::osdi::Into::CacheKeepsForeign);
        const QString cached = builds.first().library;
        QVERIFY2(cached.startsWith(inCache("good")) && cached.endsWith("/good.osdi"), qPrintable(cached));
        write(cached, osdi("good", nativeHeader()));
        netlist = netlistOf(use);
        QVERIFY2(netlist.contains("pre_osdi '" + cached + "'"), qPrintable(netlist));
        QVERIFY2(!netlist.contains("built for another platform"), qPrintable(netlist));
        QVERIFY(buildsFor(use).isEmpty());
        QCOMPARE(bytes(model), osdi("good", foreignHeader()));   // kept
        QFile::remove(cached);
    }

    // Where a source's library goes, and which is loaded: beside it; in
    // the cache - a folder for each source - when the library there is
    // another platform's (kept) or cannot be written; the one beside it
    // before the cache's, a current one before an older, this platform's
    // before another's.
    void whereTheModelGoesAndWhichIsLoaded()
    {
        using namespace qucs_s::osdi;
        const QString cache = dir.filePath("unit-cache");
        const QString base = dir.filePath("unit");
        const QString va = write(base + "/res.va", "module res(a); endmodule\n");
        const QString beside = base + "/res.osdi";
        const QDateTime now = QDateTime::currentDateTime();
        Into into = Into::CacheReadOnly;
        QCOMPARE(buildTarget(va, cache, QString(), &into), beside);
        QVERIFY(into == Into::Beside);
        QCOMPARE(modelOf(va, cache), QString());

        // Another platform's beside it: kept; this platform's into the cache.
        write(beside, osdi("res", foreignHeader()));
        const QString cached = buildTarget(va, cache, QString(), &into);
        QVERIFY(into == Into::CacheKeepsForeign);
        QVERIFY2(cached.startsWith(cache + "/osdi/res-") && cached.endsWith("/res.osdi"), qPrintable(cached));
        QCOMPARE(QFileInfo(cached).dir().dirName().size(), qsizetype(QString("res-").size() + 10));
        QCOMPARE(buildTarget(va, QString(), QString(), &into), beside);   // no cache: beside, as before
        QVERIFY(into == Into::Beside);
        QCOMPARE(modelOf(va, cache), beside);   // what there is (said, not loaded)
        write(cached, osdi("res", nativeHeader()));
        QCOMPARE(modelOf(va, cache), cached);
        QCOMPARE(modelOf(va, QString()), beside);

        // This platform's beside it, current: before the cache's.
        write(beside, osdi("res", nativeHeader()));
        QCOMPARE(buildTarget(va, cache, QString(), &into), beside);
        QVERIFY(into == Into::Beside);
        QCOMPARE(modelOf(va, cache), beside);
        // Older than the source: the cache's, current.
        setBuilt(beside, now.addSecs(-60));
        setBuilt(va, now.addSecs(-30));
        QCOMPARE(modelOf(va, cache), cached);
        // Both older: the one beside it.
        setBuilt(cached, now.addSecs(-60));
        QCOMPARE(modelOf(va, cache), beside);

        // What builds() makes of it: the one beside it is older, built again there.
        QList<Build> list = builds({va}, {}, {"res"}, QString(), cache);
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().library, beside);
        QCOMPARE(list.first().built, beside);
        QVERIFY(!list.first().missing && !list.first().foreign);
        // The cache's current: nothing to build.
        setBuilt(cached, now);
        QVERIFY(builds({va}, {}, {"res"}, QString(), cache).isEmpty());
    }

    // A folder that cannot be written - a team's library share mounted
    // read-only - or a library there that cannot: the cache. A folder in
    // the cache for each source: one of the same name elsewhere has its
    // own, the same source by another path (a link) the same.
    void aFolderThatCannotBeWrittenCompilesIntoTheCache()
    {
        using namespace qucs_s::osdi;
        const QString cache = dir.filePath("ro-cache");
        const QString base = dir.filePath("ro");
        const QString va = write(base + "/res.va", "module res(a); endmodule\n");
        const QString beside = write(base + "/res.osdi", osdi("res", nativeHeader()));
        setBuilt(beside, QDateTime::currentDateTime().addSecs(-60));   // older than its source
        Into into = Into::Beside;

        QVERIFY(QFile::setPermissions(beside, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(base, kFolder);
            QFile::setPermissions(beside, kFolder & ~(QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther));
        });
        if (QFileInfo(beside).isWritable()) QSKIP("Files that cannot be written can be here (root).");
        const QString cached = buildTarget(va, cache, QString(), &into);
        QVERIFY(into == Into::CacheReadOnly);
        QVERIFY2(cached.startsWith(cache + "/osdi/res-"), qPrintable(cached));
        QVERIFY(QFile::setPermissions(beside, kFolder));
        QCOMPARE(buildTarget(va, cache, QString(), &into), beside);
        QVERIFY(into == Into::Beside);

        QVERIFY(QFile::setPermissions(base, kReadOnlyFolder));
        QCOMPARE(buildTarget(va, cache, QString(), &into), cached);
        QVERIFY(into == Into::CacheReadOnly);
        // Older beside it, none in the cache: built, into the cache.
        QList<Build> list = builds({va}, {}, {"res"}, QString(), cache);
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().library, cached);
        QVERIFY(list.first().into == Into::CacheReadOnly);
        QCOMPARE(list.first().built, beside);
        QVERIFY(!list.first().missing);

        // The same source by a link to its folder: the same folder in the cache.
        QVERIFY(QFile::link(base, dir.filePath("ro-link")));
        QCOMPARE(buildTarget(dir.filePath("ro-link/res.va"), cache), cached);
        // Another of its name: a folder of its own.
        const QString other = write(dir.filePath("ro2/res.va"), "module res(a); endmodule\n");
        QVERIFY(QFile::setPermissions(dir.filePath("ro2"), kReadOnlyFolder));
        const QString otherCached = buildTarget(other, cache);
        QFile::setPermissions(dir.filePath("ro2"), kFolder);
        QVERIFY2(otherCached.startsWith(cache + "/osdi/res-") && otherCached != cached, qPrintable(otherCached));
    }

    // A library in a folder of the library search paths, placed by its
    // name alone (as on another computer, or with the folder moved): its
    // source compiled beside it and its model loaded from there; the
    // folder read-only, compiled into the cache and loaded from there -
    // and again when the source changes.
    void aLibraryOnASearchPathBringsItsVerilogA()
    {
        QucsSettings.EmbedVerilogAInLibraries = true;
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "vaproj";
        QucsSettings.QucsWorkDir.setPath(project);
        team = dir.filePath("team");
        {
            LibraryDialog dialog(&app);
            dialog.fillSchematicList({"sub.sch"});
            LibraryDialog::Request request;
            request.name = "TeamLib";
            request.subcircuits = {"sub.sch"};
            request.folder = team;
            QString log, error;
            QVERIFY2(dialog.create(request, &log, &error), qPrintable(error + "\n" + log));
            QVERIFY2(log.contains("Embedding Verilog-A: good.va"), qPrintable(log));
        }
        const QStringList paths = QucsSettings.LibraryPaths;
        QucsSettings.LibraryPaths = {team};
        const auto restorePaths = qScopeGuard([&] { QucsSettings.LibraryPaths = paths; });
        app.ProjName.clear();
        QucsSettings.QucsWorkDir.setPath(dir.filePath("elsewhere"));
        const QString use = write(dir.filePath("elsewhere/team.sch"), usesLibrary("TeamLib"));
        const QString folder = QFileInfo(team + "/TeamLib").canonicalFilePath();
        const QString model = folder + "/good.osdi";

        QList<qucs_s::osdi::Build> builds = buildsFor(use);
        QCOMPARE(builds.size(), 1);
        QCOMPARE(builds.first().source, folder + "/good.va");
        QCOMPARE(builds.first().library, model);
        QVERIFY(builds.first().into == qucs_s::osdi::Into::Beside);
        QVERIFY(builds.first().missing);
        write(model, osdi("good", nativeHeader()));
        QString netlist = netlistOf(use);
        QVERIFY2(netlist.contains("pre_osdi '" + model + "'"), qPrintable(netlist));
        QVERIFY(buildsFor(use).isEmpty());

        // Read-only, and no model: compiled into the cache, loaded from there.
        QFile::remove(model);
        QVERIFY(QFile::setPermissions(folder, kReadOnlyFolder));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, kFolder); });
        if (QFileInfo(folder).isWritable()) QSKIP("Folders that cannot be written can be here (root).");
        builds = buildsFor(use);
        QCOMPARE(builds.size(), 1);
        QVERIFY(builds.first().into == qucs_s::osdi::Into::CacheReadOnly);
        QVERIFY(builds.first().missing);
        const QString cached = builds.first().library;
        QVERIFY2(cached.startsWith(inCache("good")) && cached.endsWith("/good.osdi"), qPrintable(cached));
        write(cached, osdi("good", nativeHeader()));   // as OpenVAF -o writes it
        netlist = netlistOf(use);
        QVERIFY2(netlist.contains("pre_osdi '" + cached + "'"), qPrintable(netlist));
        QVERIFY(buildsFor(use).isEmpty());
        QVERIFY(!QFileInfo::exists(model));
        // The source changed (by someone who may write the folder): again.
        setBuilt(folder + "/good.va", QDateTime::currentDateTime().addSecs(60));
        builds = buildsFor(use);
        QCOMPARE(builds.size(), 1);
        QCOMPARE(builds.first().library, cached);
        QVERIFY(!builds.first().missing);
        QFile::remove(cached);
    }

    // A part's model and the files of its library's folder come from one
    // library: one beside the schematic before one of its name on the
    // search paths - the Verilog-A came from the search path's, the model
    // from the one beside it.
    void thePartsModelAndItsFilesComeFromOneLibrary()
    {
        QVERIFY(!team.isEmpty());   // aLibraryOnASearchPathBringsItsVerilogA made it
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName.clear();
        const QStringList paths = QucsSettings.LibraryPaths;
        QucsSettings.LibraryPaths = {team};
        const auto restorePaths = qScopeGuard([&] { QucsSettings.LibraryPaths = paths; });
        QucsSettings.QucsWorkDir.setPath(dir.filePath("elsewhere"));
        const QString loose = dir.filePath("loose");
        QVERIFY(QDir().mkpath(loose + "/TeamLib"));
        write(loose + "/TeamLib.lib", read(team + "/TeamLib.lib").replace(".model m1 good", ".model m1loose good").toUtf8());
        write(loose + "/TeamLib/good.va", "// this copy's\nmodule good(p, n);\nendmodule\n");
        const QString use = write(loose + "/use.sch", usesLibrary("TeamLib"));

        Schematic sch(nullptr, use);
        QVERIFY(sch.load());
        const QString here = QFileInfo(loose).canonicalFilePath();
        LibComp* part = nullptr;
        for (Component* c : sch.a_DocComps)
            if (c->Name == "X1") part = dynamic_cast<LibComp*>(c);
        QVERIFY(part != nullptr);
        QCOMPARE(part->libraryFile(), here + "/TeamLib.lib");
        QCOMPARE(part->getSubcircuitFile(), here + "/TeamLib");
        const QStringList files = AbstractSpiceKernel::collectVerilogAFiles(&sch);
        QVERIFY2(files == QStringList{here + "/TeamLib/good.va"}, qPrintable(files.join('\n')));
        const QString netlist = netlistOf(use);   // the model: that copy's too
        QVERIFY2(netlist.contains(".model m1loose good"), qPrintable(netlist));
    }

    // A library's SPICE subcircuit has the part's pins and no more, unless
    // asked for a first one, gnd; a part tells from its library which it
    // has: one with gnd (made so, or before 26.1.6) and one with a Qucs
    // model only (made SPICE with a gnd) tie it to the circuit's ground.
    void aSubcircuitHasNoGroundPinUnlessAsked()
    {
        QVERIFY(!QucsSettings.LibraryGroundPin);   // the default
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "vaproj";
        QucsSettings.QucsWorkDir.setPath(project);
        const QString pins = dir.filePath("pins");
        QString log = makeLibrary(app, "NoGnd", {"sub.sch"}, pins);
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        log = makeLibrary(app, "WithGnd", {"sub.sch"}, pins, true);
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        QVERIFY(!QucsSettings.LibraryGroundPin);   // (the request's, for that library only)

        const QStringList none = subcircuitPins(read(pins + "/NoGnd.lib"));
        const QStringList with = subcircuitPins(read(pins + "/WithGnd.lib"));
        QCOMPARE(none.size(), 2);
        QVERIFY2(!none.contains("gnd"), qPrintable(none.join(' ')));
        QCOMPARE(with.size(), 3);
        QCOMPARE(with.first(), QString("gnd"));
        QCOMPARE(with.mid(1), none);
        QVERIFY(!LibComp::takesGround(pins + "/NoGnd.lib", "sub", 2));
        QVERIFY(LibComp::takesGround(pins + "/WithGnd.lib", "sub", 2));

        app.ProjName.clear();
        QucsSettings.QucsWorkDir.setPath(dir.filePath("elsewhere"));
        // The part's two pins' nodes; with a gnd pin, ground (0) before them.
        // Nothing is wired to its pins, so a 0 is only a gnd pin's: on Linux
        // the library's symbol is narrower, and its pins fell on the grounds.
        const QStringList two = nodesOfX1(netlistOf(write(dir.filePath("elsewhere/nognd.sch"), usesLibrary(pins + "/NoGnd", false))),
                                          "NoGnd_sub");
        QCOMPARE(two.size(), 2);
        QVERIFY2(!two.contains("0"), qPrintable(two.join(' ')));
        const QStringList three = nodesOfX1(netlistOf(write(dir.filePath("elsewhere/withgnd.sch"), usesLibrary(pins + "/WithGnd", false))),
                                            "WithGnd_sub");
        QCOMPARE(three, QStringList{"0"} + two);
        // A library with a Qucs model only: made SPICE with a gnd, tied.
        QString qucsOnly = read(pins + "/NoGnd.lib");
        qucsOnly.replace("NoGnd", "QucsOnly");
        const qsizetype spice = qucsOnly.indexOf("  <Spice>"), spiceEnd = qucsOnly.indexOf("</Spice>");
        QVERIFY(spice > 0 && spiceEnd > spice);
        qucsOnly.remove(spice, spiceEnd + 9 - spice);
        write(pins + "/QucsOnly.lib", qucsOnly.toUtf8());
        QVERIFY(LibComp::takesGround(pins + "/QucsOnly.lib", "sub", 2));
        // The library changed: read again.
        write(pins + "/Changed.lib", read(pins + "/NoGnd.lib").replace("NoGnd", "Changed").toUtf8());
        QVERIFY(!LibComp::takesGround(pins + "/Changed.lib", "sub", 2));
        write(pins + "/Changed.lib", read(pins + "/WithGnd.lib").replace("WithGnd", "Changed").toUtf8() + "\n");
        QVERIFY(LibComp::takesGround(pins + "/Changed.lib", "sub", 2));
        // The component's own .SUBCKT, by the name a part gives it - not one
        // it places, before or after it.
        QCOMPARE(LibComp::subcircuitName(pins + "/Order", "sub"), QString("Order_sub"));
        QString order = read(pins + "/NoGnd.lib").replace("NoGnd", "Order");
        const qsizetype ends = order.indexOf(".ENDS");
        QVERIFY(ends > 0);
        order.insert(order.indexOf('\n', ends) + 1, ".SUBCKT inner a b c\nR1 a b 1k\n.ENDS\n");
        write(pins + "/Order.lib", order.toUtf8());
        QVERIFY(!LibComp::takesGround(pins + "/Order.lib", "sub", 2));
        // A count that fits neither: as it always was.
        write(pins + "/Odd.lib", read(pins + "/NoGnd.lib").replace("NoGnd", "Odd").replace("Odd_sub ", "Odd_sub x y ").toUtf8());
        QVERIFY(LibComp::takesGround(pins + "/Odd.lib", "sub", 2));
    }

    // The ground pin's setting: Application Settings > Settings, off unless
    // changed, kept.
    void theGroundPinSettingIsInTheSettingsDialog()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QucsSettingsDialog dialog(&app);
        auto* box = dialog.findChild<QCheckBox*>("libraryGroundPin");
        QVERIFY(box != nullptr);
        QVERIFY(!box->isChecked());
        // Create Library is in the Project menu.
        QVERIFY2(box->toolTip().startsWith("Project > Create Library"), qPrintable(box->toolTip()));
        QVERIFY2(dialog.findChild<QCheckBox*>("embedVerilogA")->toolTip().startsWith("Project > Create Library"),
                 qPrintable(dialog.findChild<QCheckBox*>("embedVerilogA")->toolTip()));
        QVERIFY(!_settings::Get().itemDefault<bool>("LibraryGroundPin"));
        box->setChecked(true);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "slotApply"));
        QVERIFY(QucsSettings.LibraryGroundPin);
        QVERIFY(_settings::Get().item<bool>("LibraryGroundPin"));
        QucsSettings.LibraryGroundPin = false;
        saveApplSettings();
        QVERIFY(!_settings::Get().item<bool>("LibraryGroundPin"));
    }

    // Document Settings > Library marks a subcircuit: saved with it -
    // <AlwaysLoadOSDI=1>, only when set (a Qucs-S that does not know it
    // refuses the file) - and read back.
    void theDocumentSettingsMarkASubcircuit()
    {
        Module::registerModules();   // (a QucsApp's destructor unregisters them)
        const QString file = write(project + "/marked.sch", read(project + "/sub.sch").toUtf8());
        Schematic sch(nullptr, file);
        QVERIFY(sch.load());
        QVERIFY(!sch.getAlwaysLoadOSDI());
        {
            SettingsDialog dialog(&sch);
            auto* box = dialog.findChild<QCheckBox*>("alwaysLoadOSDI");
            QVERIFY(box != nullptr);
            QVERIFY(!box->isChecked());
            box->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotApply"));
        }
        QVERIFY(sch.getAlwaysLoadOSDI());
        QVERIFY(sch.getDocChanged());
        QVERIFY(sch.save() >= 0);
        QVERIFY2(read(file).contains("\n  <AlwaysLoadOSDI=1>\n"), qPrintable(read(file)));
        {
            Schematic again(nullptr, file);
            QVERIFY(again.load());
            QVERIFY(again.getAlwaysLoadOSDI());
            SettingsDialog dialog(&again);
            QVERIFY(dialog.findChild<QCheckBox*>("alwaysLoadOSDI")->isChecked());
        }
        sch.setAlwaysLoadOSDI(false);
        QVERIFY(sch.save() >= 0);
        QVERIFY2(!read(file).contains("AlwaysLoadOSDI"), qPrintable(read(file)));
        // Read again into one that had it: not marked, as the file says.
        sch.setAlwaysLoadOSDI(true);
        QVERIFY(sch.load());
        QVERIFY(!sch.getAlwaysLoadOSDI());
        QFile::remove(file);
    }

    // A part marked so (its subcircuit's Document Settings > Library):
    // every circuit of a project that has its library - here the project's
    // own - loads its Verilog-A, placed or not; a circuit elsewhere does
    // not, nor does one when the mark is gone. Marked with no Verilog-A in the
    // library: said, as loading nothing.
    void aMarkedPartsVerilogAIsLoadedInTheProjectsCircuits()
    {
        QucsSettings.EmbedVerilogAInLibraries = true;
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "vaproj";
        QucsSettings.QucsWorkDir.setPath(project);
        write(project + "/always.va", "`include \"disciplines.vams\"\nmodule always(p, n);\nendmodule\n");
        write(project + "/always.sch", read(project + "/sub.sch").replace(".model m1 good", ".model m3 always")
                                          .replace("<Components>", "<Properties>\n  <AlwaysLoadOSDI=1>\n</Properties>\n<Components>")
                                          .toUtf8());
        QString log = makeLibrary(app, "MarkLib", {"sub.sch", "always.sch"}, project);
        QVERIFY2(log.contains("Successfully created library."), qPrintable(log));
        QVERIFY2(log.contains("Marked: every circuit of a project that has the library loads its Verilog-A models."), qPrintable(log));
        QCOMPARE(LibComp::alwaysLoaded(project + "/MarkLib.lib"), QStringList{"always"});
        QVERIFY(QFileInfo::exists(project + "/MarkLib/always.va"));
        QCOMPARE(qucs_s::projectlibraries::alwaysLoadedModules(project), QSet<QString>{"always"});
        // Its source changed: read again (each is read once while unchanged).
        const QByteArray source = read(project + "/MarkLib/always.va").toUtf8();
        write(project + "/MarkLib/always.va", QByteArray(source).replace("module always(", "module renamed(p, q, "));
        QCOMPARE(qucs_s::projectlibraries::alwaysLoadedModules(project), QSet<QString>{"renamed"});
        write(project + "/MarkLib/always.va", source);
        QCOMPARE(qucs_s::projectlibraries::alwaysLoadedModules(project), QSet<QString>{"always"});

        // A circuit of the project that places none of it: always compiled
        // and loaded; good, which it does not use, not.
        const QString plain = write(project + "/plain.sch",
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <R R1 1 100 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <GND * 1 70 100 0 0 0 0>\n  <GND * 1 130 100 0 0 0 0>\n"
            "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        QStringList compiled;
        for (const qucs_s::osdi::Build& b : buildsFor(plain)) compiled << QFileInfo(b.source).fileName();
        QVERIFY2(compiled.contains("always.va") && !compiled.contains("good.va"), qPrintable(compiled.join(' ')));
        write(project + "/always.osdi", osdi("always", nativeHeader()));
        write(project + "/MarkLib/always.osdi", osdi("always", nativeHeader()));
        QString netlist = netlistOf(plain);
        QVERIFY2(netlist.contains("pre_osdi '") && netlist.contains("always.osdi'"), qPrintable(netlist));
        QVERIFY2(!netlist.contains("good.osdi"), qPrintable(netlist));

        // Not a circuit of the project: not loaded.
        app.ProjName.clear();
        QucsSettings.QucsWorkDir.setPath(dir.filePath("elsewhere"));
        const QString outside = write(dir.filePath("elsewhere/plain.sch"), read(plain).toUtf8());
        QVERIFY2(!netlistOf(outside).contains("always.osdi"), qPrintable(netlistOf(outside)));
        app.ProjName = "vaproj";
        QucsSettings.QucsWorkDir.setPath(project);
        QVERIFY2(!netlistOf(outside).contains("always.osdi"), qPrintable(netlistOf(outside)));   // (another folder's)

        // The mark gone from the library: not loaded.
        write(project + "/MarkLib.lib", read(project + "/MarkLib.lib").remove("  <AlwaysLoadOSDI>\n").toUtf8());
        QVERIFY(LibComp::alwaysLoaded(project + "/MarkLib.lib").isEmpty());
        netlist = netlistOf(plain);
        QVERIFY2(!netlist.contains("always.osdi"), qPrintable(netlist));

        // Marked, with no Verilog-A embedded: made, and said to load nothing.
        QucsSettings.EmbedVerilogAInLibraries = false;
        log = makeLibrary(app, "BareMark", {"always.sch"}, dir.filePath("bare"));
        QucsSettings.EmbedVerilogAInLibraries = true;
        QVERIFY2(log.contains("Successfully created library.") && log.contains("the mark loads nothing"), qPrintable(log));
        QCOMPARE(LibComp::alwaysLoaded(dir.filePath("bare/BareMark.lib")), QStringList{"always"});

        for (const QString& f : {project + "/always.va", project + "/always.sch", project + "/always.osdi", plain,
                                 project + "/MarkLib.lib"})
            QFile::remove(f);
        QDir(project + "/MarkLib").removeRecursively();
    }

    // The setting: Application Settings > Settings, kept.
    void theSettingIsInTheSettingsDialog()
    {
        QucsSettings.EmbedVerilogAInLibraries = true;
        QucsApp app(false);
        MainGuard guard(&app);
        QucsSettingsDialog dialog(&app);
        auto* box = dialog.findChild<QCheckBox*>("embedVerilogA");
        QVERIFY(box != nullptr);
        QVERIFY(box->isChecked());
        box->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "slotApply"));
        QVERIFY(!QucsSettings.EmbedVerilogAInLibraries);
        QVERIFY(!_settings::Get().item<bool>("EmbedVerilogAInLibraries"));
        QucsSettings.EmbedVerilogAInLibraries = true;
        saveApplSettings();
    }
};

QTEST_MAIN(TestLibraryVerilogA)
#include "test_library_verilog_a.moc"
