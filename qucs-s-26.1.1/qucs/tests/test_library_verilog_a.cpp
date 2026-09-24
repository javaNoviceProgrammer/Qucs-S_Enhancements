/*
 * Libraries that bring their Verilog-A: Tools > Create Library copies the
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
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "osdiselection.h"
#include "schematic.h"
#include "settings.h"
#include "dialogs/librarydialog.h"
#include "dialogs/qucssettingsdialog.h"
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
    QString workspace, project, userLib;

    // Tools > Create Library with the subcircuit \a schematic, no
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
        const QString use = write(dir.filePath("elsewhere/use.sch"),
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <Lib X1 1 100 100 20 -20 0 0 \"" + userLib.toUtf8() + "/VaLib\" 0 \"sub\" 0>\n"
            "  <GND * 1 70 100 0 0 0 0>\n  <GND * 1 130 100 0 0 0 0>\n"
            "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        QucsSettings.QucsWorkDir.setPath(dir.filePath("elsewhere"));
        // The library found through its canonical path (/private/var for
        // /var on macOS).
        const QString model = QFileInfo(userLib + "/VaLib").canonicalFilePath() + "/good.osdi";
        const auto buildsFor = [&]() {
            Schematic sch(nullptr, use);
            if (!sch.load()) return QList<qucs_s::osdi::Build>();
            Ngspice kernel(&sch);
            kernel.setWorkdir(dir.filePath("kernel"));
            return kernel.verilogABuilds();
        };

        // Not compiled yet: compiled, beside the source, before it runs.
        QString netlist = netlistOf(use);
        QVERIFY2(!netlist.contains("pre_osdi"), qPrintable(netlist));
        QVERIFY2(netlist.contains(".model m1 good"), qPrintable(netlist));
        QList<qucs_s::osdi::Build> builds = buildsFor();
        QCOMPARE(builds.size(), 1);
        QCOMPARE(QFileInfo(builds.first().source).fileName(), QString("good.va"));
        QCOMPARE(builds.first().library, model);
        QVERIFY(builds.first().missing);
        QVERIFY(!builds.first().foreign);

        // Compiled (as OpenVAF would): loaded, and not compiled again.
        write(model, osdi("good", nativeHeader()));
        netlist = netlistOf(use);
        QVERIFY2(netlist.contains("pre_osdi '" + model + "'"), qPrintable(netlist));
        QVERIFY(buildsFor().isEmpty());   // the model is newer than its source

        // The model as it came from another platform.
        write(model, osdi("good", foreignHeader()));
        netlist = netlistOf(use);
        QVERIFY2(!netlist.contains("pre_osdi"), qPrintable(netlist));
        QVERIFY2(netlist.contains("* OSDI: ") && netlist.contains("built for another platform"), qPrintable(netlist));
        builds = buildsFor();
        QCOMPARE(builds.size(), 1);
        QCOMPARE(builds.first().library, model);
        QVERIFY(builds.first().foreign);
        QVERIFY(!builds.first().missing);
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
