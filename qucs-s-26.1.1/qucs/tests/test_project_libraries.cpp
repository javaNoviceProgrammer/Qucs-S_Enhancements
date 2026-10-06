/*
 * The Verilog-A of the library devices a project's schematics use, in the
 * project (projectlibraries.h): a folder named after each library with a
 * link to each source (a copy where links are not made), made when a
 * schematic uses a device - saved, open, or below a subcircuit outside
 * the project -, made again when it leads elsewhere, taken away with the
 * model compiled beside it when none does; only what Qucs-S put there.
 * The model compiled beside the link and loaded from there, the library's
 * folder untouched; the link opened read-only, said whose in the Content
 * panel; made when the project opens and before a simulation compiles.
 */
#include <QtTest>
#include <QCryptographicHash>
#include <QMessageBox>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "erc.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "osdiselection.h"
#include "projectView.h"
#include "projectlibraries.h"
#include "qucs.h"
#include "schematic.h"
#include "textdoc.h"
#include "dialogs/librarydialog.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "components/libcomp.h"

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

using namespace qucs_s;
using projectlibraries::Mode;

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

QByteArray bytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString real(const QString& path) { return QFileInfo(path).canonicalFilePath(); }

void setBuilt(const QString& path, const QDateTime& when)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(when, QFileDevice::FileModificationTime));
}

// A schematic with parts of the libraries \a libs (each a Lib: a path or a
// name), component "sub", and a DC analysis.
QByteArray usesLibraries(const QStringList& libs)
{
    QByteArray parts;
    int n = 0;
    for (const QString& lib : libs) {
        ++n;   // (X1 at x 200, X2 at 300...: n read once in each line - GCC had X2 first)
        parts += "  <Lib X" + QByteArray::number(n) + " 1 " + QByteArray::number(100 * (n + 1)) + " 100 20 -20 0 0 \""
                 + lib.toUtf8() + "\" 0 \"sub\" 0>\n";
    }
    return "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n" + parts +
           "  <GND * 1 70 100 0 0 0 0>\n"
           "  <.DC DC1 1 500 200 0 40 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 "
           "\"CroutLU\" 0 \"no\" 0>\n"
           "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
}
QByteArray usesLibrary(const QString& lib) { return usesLibraries({lib}); }
// Parts of the components \a comps of the library \a lib.
QByteArray usesParts(const QString& lib, const QStringList& comps)
{
    QByteArray parts;
    int n = 0;
    for (const QString& comp : comps) {
        ++n;
        parts += "  <Lib X" + QByteArray::number(n) + " 1 " + QByteArray::number(100 * (n + 1)) + " 100 20 -20 0 0 \""
                 + lib.toUtf8() + "\" 0 \"" + comp.toUtf8() + "\" 0>\n";
    }
    return "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n" + parts +
           "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
}
QByteArray plain() { return usesLibraries({}); }
// A library NAME of one part, \a comp: a resistor of two pins.
QByteArray libraryOf(const QString& name, const QString& comp)
{
    return "<Qucs Library " PACKAGE_VERSION " \"" + name.toUtf8() + "\">\n\n<Component " + comp.toUtf8() + ">\n"
           "  <Description>\nA resistor\n  </Description>\n  <Model>\n.Def:" + name.toUtf8() + "_" + comp.toUtf8() +
           " _net0 _net1\nR:R1 _net0 _net1 R=\"1k\"\n.Def:End\n  </Model>\n  <Symbol>\n    <.PortSym -30 0 1 0 P1>\n"
           "    <.PortSym 30 0 2 180 P2>\n    <Line -30 0 60 0 #000080 2 1>\n  </Symbol>\n</Component>\n";
}
QString native(const QString& path) { return QDir::toNativeSeparators(path); }
// What the check says of the schematic, a line each.
QString issuesOf(Schematic& sch)
{
    QStringList lines;
    for (const erc::Issue& i : erc::check(&sch)) lines << i.message;
    return lines.join('\n');
}
LibComp* partNamed(Schematic& sch, const QString& name)
{
    for (Component* c : sch.a_DocComps)
        if (c->Name == name) return dynamic_cast<LibComp*>(c);
    return nullptr;
}

// A file row of the Content panel, by its path relative to the project.
QModelIndex rowOf(QStandardItemModel* model, const QString& path, const QModelIndex& parent = QModelIndex())
{
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex idx = model->index(row, 0, parent);
        if (idx.data(ProjectView::FilePathRole).toString() == path) return idx;
        const QModelIndex inside = rowOf(model, path, idx);
        if (inside.isValid()) return inside;
    }
    return {};
}
} // namespace

class TestProjectLibraries : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString workspace, team, other, source;

    QString project(const QString& name)
    {
        const QString path = workspace + "/" + name + "_prj";
        QDir().mkpath(path);
        return path;
    }

    // VaLib - good.va, which includes inc/common.vams - made from the
    // source project into \a folder.
    void makeLibrary(QucsApp& app, const QString& folder)
    {
        LibraryDialog dialog(&app);
        dialog.fillSchematicList({"sub.sch"});
        LibraryDialog::Request request;
        request.name = "VaLib";
        request.subcircuits = {"sub.sch"};
        request.folder = folder;
        QString log, error;
        QVERIFY2(dialog.create(request, &log, &error), qPrintable(error + "\n" + log));
        QVERIFY2(log.contains("Embedding Verilog-A: good.va, inc/common.vams"), qPrintable(log));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.EmbedVerilogAInLibraries = true;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QDir().mkpath(dir.filePath("kernel"));
        QucsSettings.S4Qworkdir = dir.filePath("kernel");

        workspace = dir.filePath("workspace");
        team = dir.filePath("team");
        other = dir.filePath("other");
        source = workspace + "/src_prj";
        QVERIFY(QDir().mkpath(source));
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
        QucsSettings.projsDir.setPath(workspace);
        QucsSettings.QucsWorkDir.setPath(source);
        write(source + "/good.va", "`include \"disciplines.vams\"\n`include \"inc/common.vams\"\n"
                                   "module good(p, n);\nendmodule\n");
        write(source + "/inc/common.vams", "// shared\n");
        write(source + "/better.va", "`include \"disciplines.vams\"\nmodule better(p, n);\nendmodule\n");
        write(source + "/sub2.sch",
              "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
              "  <Port P1 1 220 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
              "  <Port P2 1 280 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n"
              "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
              "  <SpiceModel SpiceModel1 1 120 300 -27 16 0 0 \".model m2 better\" 1 \"\" 0>\n"
              "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        write(source + "/sub.sch",
              "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
              "  <Port P1 1 220 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
              "  <Port P2 1 280 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n"
              "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
              "  <SpiceModel SpiceModel1 1 120 300 -27 16 0 0 \".model m1 good\" 1 \"\" 0>\n"
              "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "src";
        makeLibrary(app, team);
        makeLibrary(app, other);   // another library of the name, elsewhere
        {
            // TwoLib: sub (good.va) and sub2 (better.va).
            LibraryDialog dialog(&app);
            dialog.fillSchematicList({"sub.sch", "sub2.sch"});
            LibraryDialog::Request request;
            request.name = "TwoLib";
            request.subcircuits = {"sub.sch", "sub2.sch"};
            request.folder = team;
            QString log, error;
            QVERIFY2(dialog.create(request, &log, &error), qPrintable(error + "\n" + log));
        }
        app.ProjName.clear();
        QucsSettings.LibraryPaths = {team};
        Module::registerModules();   // (a QucsApp's destructor unregisters them)
    }

    // A schematic of the project placing a device - by its library's name,
    // found on the library search paths - has its source linked in, and
    // the record says so; nothing else, and nothing in the library's folder.
    void aDeviceUsedHasItsSourceLinked()
    {
        const QString p = project("p1");
        write(p + "/use.sch", usesLibrary("VaLib"));
        write(p + "/plain.sch", plain());
        const QStringList teamFiles = QDir(team + "/VaLib").entryList(QDir::Files | QDir::Hidden);
        const projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.made, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(r.removed.isEmpty() && r.conflicts.isEmpty());
        const QFileInfo link(p + "/Libraries/VaLib/good.va");
        QVERIFY(link.isSymLink());
        QCOMPARE(real(link.symLinkTarget()), real(team + "/VaLib/good.va"));
        QVERIFY(QFileInfo::exists(p + "/Libraries/VaLib/" + projectlibraries::RecordName));
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib/inc"));   // its includes: OpenVAF finds them beside the original
        const projectlibraries::Entry e = projectlibraries::entryOf(p + "/Libraries/VaLib/good.va");
        QCOMPARE(e.library, QString("VaLib"));
        QCOMPARE(real(e.original), real(team + "/VaLib/good.va"));
        QVERIFY(projectlibraries::entryOf(p + "/use.sch").isEmpty());
        QVERIFY(projectlibraries::entryOf(p + "/Libraries/VaLib/" + projectlibraries::RecordName).isEmpty());
        // Again: as it is.
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QCOMPARE(QDir(team + "/VaLib").entryList(QDir::Files | QDir::Hidden), teamFiles);
        // The project lists it as one of its sources.
        QVERIFY(misc::projectFiles(QDir(p), {"*.va"}).contains("Libraries/VaLib/good.va"));
    }

    // Kept while a schematic uses it; then taken away with the model
    // compiled beside it, the record, and the folder Qucs-S made.
    void noLongerUsedItIsTakenAwayWithItsModel()
    {
        const QString p = project("p1");
        write(p + "/also.sch", usesLibrary("VaLib"));
        write(p + "/Libraries/VaLib/good.osdi", "a model");
        write(p + "/use.sch", plain());
        projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QVERIFY(!r.changed());   // also.sch uses it
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        QFile::remove(p + "/also.sch");
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.removed, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(!QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink() && !QFileInfo::exists(p + "/Libraries/VaLib/good.va"));
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib/good.osdi"));
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib"));
        QVERIFY(QFileInfo::exists(team + "/VaLib/good.va"));   // the original, of course
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
    }

    // A folder of the user's with the library's name: a file of theirs in
    // the way is left as it is (and not taken away later), the folder and
    // what else is in it stay.
    void theUsersFilesAreNeverTouched()
    {
        const QString p = project("p2");
        write(p + "/Libraries/VaLib/notes.txt", "mine");
        write(p + "/Libraries/VaLib/good.va", "// my own good.va\n");
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.conflicts, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(r.made.isEmpty());
        QCOMPARE(bytes(p + "/Libraries/VaLib/good.va"), QByteArray("// my own good.va\n"));
        r = projectlibraries::sync(p, {}, Mode::Copy);   // (where copies are made: the same)
        QCOMPARE(r.conflicts, QStringList{"Libraries/VaLib/good.va"});
        QCOMPARE(bytes(p + "/Libraries/VaLib/good.va"), QByteArray("// my own good.va\n"));
        QVERIFY(projectlibraries::entryOf(p + "/Libraries/VaLib/good.va").isEmpty());
        write(p + "/use.sch", plain());
        projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(bytes(p + "/Libraries/VaLib/good.va"), QByteArray("// my own good.va\n"));
        // Out of the way: linked; taken away again - the folder was the user's.
        QFile::remove(p + "/Libraries/VaLib/good.va");
        write(p + "/use.sch", usesLibrary("VaLib"));
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.made, QStringList{"Libraries/VaLib/good.va"});
        // A link of the user's, beside it: theirs.
        QVERIFY(QFile::link(team + "/VaLib.lib", p + "/Libraries/VaLib/mine.lib"));
        write(p + "/use.sch", plain());
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.removed, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(QFileInfo::exists(p + "/Libraries/VaLib/notes.txt"));
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/mine.lib").isSymLink());
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib/" + projectlibraries::RecordName));
        // A link it had made, replaced by a file of the user's: theirs now.
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::sync(p, {}, Mode::Link);
        QFile::remove(p + "/Libraries/VaLib/good.va");
        write(p + "/Libraries/VaLib/good.va", "// edited\n");
        QVERIFY(projectlibraries::entryOf(p + "/Libraries/VaLib/good.va").isEmpty());   // (opened as the user's, editable)
        write(p + "/use.sch", plain());
        projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(bytes(p + "/Libraries/VaLib/good.va"), QByteArray("// edited\n"));
    }

    // A library a part names that is not found here (another computer):
    // what it has is kept.
    void aLibraryNotFoundHereKeepsWhatItHas()
    {
        const QString p = project("p3");
        write(p + "/use.sch", usesLibrary("VaLib"));
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
        write(p + "/Libraries/VaLib/good.osdi", "a model");
        QucsSettings.LibraryPaths.clear();
        const auto restore = qScopeGuard([&] { QucsSettings.LibraryPaths = {team}; });
        const projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QVERIFY(!r.changed());
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        QVERIFY(QFileInfo::exists(p + "/Libraries/VaLib/good.osdi"));
    }

    // The library moved (or the project went to another computer, which
    // has it elsewhere): the link made again to where it is now, the model
    // compiled from the other file taken away.
    void aLinkThatLeadsElsewhereIsMadeAgain()
    {
        const QString a = dir.filePath("teamA"), b = dir.filePath("teamB");
        for (const QString& t : {a, b}) {
            QVERIFY(QDir().mkpath(t + "/VaLib/inc"));
            QVERIFY(QFile::copy(team + "/VaLib.lib", t + "/VaLib.lib"));
            QVERIFY(QFile::copy(team + "/VaLib/good.va", t + "/VaLib/good.va"));
            QVERIFY(QFile::copy(team + "/VaLib/inc/common.vams", t + "/VaLib/inc/common.vams"));
        }
        const auto restore = qScopeGuard([&] { QucsSettings.LibraryPaths = {team}; });
        const QString p = project("p4");
        write(p + "/use.sch", usesLibrary("VaLib"));
        QucsSettings.LibraryPaths = {a};
        projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(real(QFileInfo(p + "/Libraries/VaLib/good.va").symLinkTarget()), real(a + "/VaLib/good.va"));
        write(p + "/Libraries/VaLib/good.osdi", "compiled from teamA's");
        QucsSettings.LibraryPaths = {b};
        QVERIFY(QDir(a).removeRecursively());   // (a link that leads nowhere now)
        const projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.made, QStringList{"Libraries/VaLib/good.va"});
        QCOMPARE(real(QFileInfo(p + "/Libraries/VaLib/good.va").symLinkTarget()), real(b + "/VaLib/good.va"));
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib/good.osdi"));
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib_2"));   // the same library's folder
    }

    // What counts: a subcircuit outside the project placing a device; not
    // a schematic of the Scratch folder; a schematic open with unsaved
    // changes, as it is.
    void whatCountsAsUsed()
    {
        const QString outside = write(dir.filePath("outside/sub_lib.sch"), usesLibrary("VaLib"));
        const QString p = project("p5");
        write(p + "/top.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
                              "  <Sub SUB1 1 260 130 26 -20 0 1 \"" + outside.toUtf8() + "\" 0>\n"
                              "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});

        const QString q = project("p6");
        write(q + "/Scratch/copy.sch", usesLibrary("VaLib"));
        QVERIFY(!projectlibraries::sync(q, {}, Mode::Link).changed());

        const QString o = project("p7");
        const QString file = write(o + "/new.sch", usesLibrary("VaLib"));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic sch(nullptr, file);
        QVERIFY(sch.load());
        write(file, plain());   // saved without it; in memory with it
        QCOMPARE(projectlibraries::sync(o, {&sch}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
        QCOMPARE(projectlibraries::sync(o, {}, Mode::Link).removed, QStringList{"Libraries/VaLib/good.va"});
        // One open placing a subcircuit outside the project that places one.
        const QString file2 = write(o + "/new2.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
                                    "  <Sub SUB1 1 260 130 26 -20 0 1 \"" + outside.toUtf8() + "\" 0>\n"
                                    "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        Schematic sch2(nullptr, file2);
        QVERIFY(sch2.load());
        write(file2, plain());
        QCOMPARE(projectlibraries::sync(o, {&sch2}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
        QCOMPARE(projectlibraries::sync(o, {}, Mode::Link).removed, QStringList{"Libraries/VaLib/good.va"});
        // A part whose library is not found: kept as unresolved, no folder.
        QList<projectlibraries::Use> uses;
        QStringList unresolved;
        write(o + "/lost.sch", usesLibrary("/nowhere/Lost"));
        projectlibraries::usedSources(o, {}, &uses, &unresolved);
        QVERIFY(uses.isEmpty());
        QCOMPARE(unresolved, QStringList{"Lost"});
    }

    // The project's own library (NAME.lib and its folder in the project):
    // its files are the project's already - nothing linked; a library of
    // that name from elsewhere goes into Libraries/, apart from it.
    void aLibraryInTheProjectIsNotLinked()
    {
        const QString p = project("p8");
        QVERIFY(QDir().mkpath(p + "/VaLib/inc"));
        QVERIFY(QFile::copy(team + "/VaLib.lib", p + "/VaLib.lib"));
        QVERIFY(QFile::copy(team + "/VaLib/good.va", p + "/VaLib/good.va"));
        write(p + "/use.sch", usesLibrary("VaLib"));   // beside the schematic: the project's
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QVERIFY(!QFileInfo(p + "/VaLib/good.va").isSymLink());
        QVERIFY(!QFileInfo::exists(p + "/Libraries"));
        // One of that name from elsewhere: in Libraries/, apart from the project's own.
        write(p + "/use.sch", usesLibrary(team + "/VaLib"));
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
        QCOMPARE(bytes(p + "/VaLib/good.va"), bytes(team + "/VaLib/good.va"));
        QVERIFY(!QFileInfo(p + "/VaLib/good.va").isSymLink());
    }

    // Two libraries of one name, from two folders: a folder each, the
    // same ones each time.
    void twoLibrariesOfOneNameGetAFolderEach()
    {
        const QString p = project("p9");
        write(p + "/use.sch", usesLibraries({team + "/VaLib", other + "/VaLib"}));
        projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QStringList made = r.made;
        made.sort();
        QCOMPARE(made, (QStringList{"Libraries/VaLib/good.va", "Libraries/VaLib_2/good.va"}));
        const QStringList targets{real(QFileInfo(p + "/Libraries/VaLib/good.va").symLinkTarget()),
                                  real(QFileInfo(p + "/Libraries/VaLib_2/good.va").symLinkTarget())};
        QVERIFY(targets.contains(real(team + "/VaLib/good.va")) && targets.contains(real(other + "/VaLib/good.va")));
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        // One of them no longer used: its folder goes, the other's stays.
        write(p + "/use.sch", usesLibrary(team + "/VaLib"));
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.removed.size(), 1);
        QVERIFY(!QFileInfo::exists(QFileInfo(p + "/" + r.removed.first()).absolutePath()));   // its folder gone
    }

    // Where links are not made (Windows): a copy of the source and of the
    // files it includes, renewed when the original changes; a link of
    // before replaced by one, and back; all taken away when unused.
    void copiesWhereLinksAreNotMade()
    {
        const QString p = project("p10");
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Copy);
        QVERIFY2(r.made == QStringList{"Libraries/VaLib/good.va"}, qPrintable(r.made.join(',') + " | " + r.conflicts.join(',')));
        QVERIFY(!QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        QCOMPARE(bytes(p + "/Libraries/VaLib/good.va"), bytes(team + "/VaLib/good.va"));
        QCOMPARE(bytes(p + "/Libraries/VaLib/inc/common.vams"), bytes(team + "/VaLib/inc/common.vams"));
        QCOMPARE(projectlibraries::entryOf(p + "/Libraries/VaLib/good.va").library, QString("VaLib"));
        QCOMPARE(projectlibraries::entryOf(p + "/Libraries/VaLib/inc/common.vams").library, QString("VaLib"));
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Copy).changed());
        // The original changed: renewed.
        const QByteArray was = bytes(team + "/VaLib/good.va");
        const auto restore = qScopeGuard([&] { write(team + "/VaLib/good.va", was); });
        write(team + "/VaLib/good.va", was + "// changed\n");
        setBuilt(team + "/VaLib/good.va", QDateTime::currentDateTime().addSecs(60));
        r = projectlibraries::sync(p, {}, Mode::Copy);
        QCOMPARE(r.made, QStringList{"Libraries/VaLib/good.va"});
        QCOMPARE(bytes(p + "/Libraries/VaLib/good.va"), was + "// changed\n");
        // Links now: the copy and its include replaced, the include taken away.
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.made, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib/inc"));
        // Copies again - a file it includes from outside the library's folder
        // is not copied (nothing goes outside the project's folder of it).
        write(team + "/outside.vams", "// beside the library\n");
        write(team + "/VaLib/good.va", was + "`include \"../outside.vams\"\n");
        setBuilt(team + "/VaLib/good.va", QDateTime::currentDateTime().addSecs(90));
        projectlibraries::sync(p, {}, Mode::Copy);
        QVERIFY(QFileInfo::exists(p + "/Libraries/VaLib/inc/common.vams"));
        QVERIFY(!QFileInfo::exists(p + "/outside.vams") && !QFileInfo::exists(workspace + "/outside.vams"));
        QVERIFY(bytes(p + "/Libraries/VaLib/good.va").contains("outside.vams"));
        write(p + "/use.sch", plain());
        r = projectlibraries::sync(p, {}, Mode::Copy);
        QCOMPARE(r.removed, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib"));
    }

    // The model is compiled beside the link and loaded from there - not
    // from the library's folder, where nothing is written -, and again when
    // a file the original includes changes; a copy's the same way.
    void theModelIsCompiledBesideTheLink()
    {
        const QString p = project("p11");
        write(p + "/use.sch", usesLibrary("VaLib"));
        QucsApp app(false);
        MainGuard guard(&app);
        app.openProject(p);   // links what it uses
        QCOMPARE(app.lastLibrarySync().made, QStringList{"Libraries/VaLib/good.va"});
        const auto buildsOf = [&](const QString& file) {
            Schematic sch(nullptr, file);
            if (!sch.load()) return QList<osdi::Build>();
            Ngspice kernel(&sch);
            kernel.setWorkdir(dir.filePath("kernel"));
            return kernel.verilogABuilds();
        };
        // A model of the library's file in the cache (build_verilog_a of it,
        // its folder read-only): the project's link is compiled for itself.
        const QString shared = team + "/VaLib";
        const QFileDevice::Permissions perms = QFile::permissions(shared);
        QVERIFY(QFile::setPermissions(shared, perms & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser
                                                       | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
        osdi::Into into = osdi::Into::Beside;
        const QString libraryModel = osdi::buildTarget(shared + "/good.va", misc::cacheDir(), QString(), &into);
        QFile::setPermissions(shared, perms);
        if (into == osdi::Into::CacheReadOnly) {   // (not as root)
            write(libraryModel, QByteArray(64, '\0') + "good" + QByteArray(1, '\0'));
            QVERIFY(osdi::modelOf(p + "/Libraries/VaLib/good.va", misc::cacheDir()).isEmpty());
        }
        QList<osdi::Build> builds = buildsOf(p + "/use.sch");
        QCOMPARE(builds.size(), 1);   // the link alone, not the library's source too
        QCOMPARE(builds.first().source, QFileInfo(p + "/Libraries/VaLib/good.va").absoluteFilePath());
        QCOMPARE(builds.first().library, QFileInfo(p + "/Libraries/VaLib/good.osdi").absoluteFilePath());
        QVERIFY(builds.first().into == osdi::Into::Beside);
        // Compiled (as OpenVAF does): loaded from there.
        write(p + "/Libraries/VaLib/good.osdi", QByteArray(64, '\0') + "good" + QByteArray(1, '\0'));
        Schematic sch(nullptr, p + "/use.sch");
        QVERIFY(sch.load());
        Ngspice kernel(&sch);
        kernel.setWorkdir(dir.filePath("kernel"));
        kernel.SaveNetlist(dir.filePath("kernel/p11.cir"), false);
        const QString netlist = QString::fromUtf8(bytes(dir.filePath("kernel/p11.cir")));
        QVERIFY2(netlist.contains("pre_osdi '" + QFileInfo(p + "/Libraries/VaLib/good.osdi").absoluteFilePath() + "'"), qPrintable(netlist));
        QVERIFY(buildsOf(p + "/use.sch").isEmpty());
        QVERIFY(!QFileInfo::exists(team + "/VaLib/good.osdi"));
        // One in the library's folder (compiled there before, with no project),
        // built later: the project's is loaded all the same.
        write(team + "/VaLib/good.osdi", QByteArray(64, '\0') + "good" + QByteArray(1, '\0'));
        setBuilt(team + "/VaLib/good.osdi", QDateTime::currentDateTime().addSecs(30));
        kernel.SaveNetlist(dir.filePath("kernel/p11b.cir"), false);
        const QString again = QString::fromUtf8(bytes(dir.filePath("kernel/p11b.cir")));
        QVERIFY2(again.contains("pre_osdi '" + QFileInfo(p + "/Libraries/VaLib/good.osdi").absoluteFilePath() + "'")
                     && !again.contains(real(team) + "/VaLib/good.osdi"), qPrintable(again));
        QFile::remove(team + "/VaLib/good.osdi");
        // A file the original includes changed: compiled again.
        setBuilt(team + "/VaLib/inc/common.vams", QDateTime::currentDateTime().addSecs(120));
        QCOMPARE(buildsOf(p + "/use.sch").size(), 1);
        setBuilt(team + "/VaLib/inc/common.vams", QDateTime::currentDateTime().addSecs(-3600));
        // A copy (Windows): the copy alone is compiled (the library's file,
        // no model of it anywhere, not too).
        if (into == osdi::Into::CacheReadOnly) QFile::remove(libraryModel);
        projectlibraries::sync(p, {}, Mode::Copy);
        QFile::remove(p + "/Libraries/VaLib/good.osdi");
        builds = buildsOf(p + "/use.sch");
        QCOMPARE(builds.size(), 1);
        QCOMPARE(builds.first().source, QFileInfo(p + "/Libraries/VaLib/good.va").absoluteFilePath());
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
    }

    // Opened in the editor read-only, a note saying whose it is; not saved
    // (the library's original stays); saved as another file, editable.
    void theEditorShowsItReadOnly()
    {
        const QString p = project("p12");
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::sync(p, {}, Mode::Link);
        QucsApp app(false);
        MainGuard guard(&app);
        TextDoc doc(&app, p + "/Libraries/VaLib/good.va");
        QVERIFY(doc.load());
        QVERIFY(doc.isReadOnly());
        QCOMPARE(doc.libraryOrigin(), QString("VaLib"));
        auto* note = doc.findChild<QLabel*>("libraryOrigin");
        QVERIFY(note != nullptr);
        QVERIFY(!note->isHidden());
        QVERIFY(note->text().contains("VaLib") && note->text().contains("read-only"));
        QVERIFY(note->toolTip().contains(QDir::toNativeSeparators(real(team + "/VaLib/good.va"))));
        const QByteArray original = bytes(team + "/VaLib/good.va");
        doc.replace("module", "MODULE", false, true, false, false);
        doc.selectAll();
        doc.commentSelected();
        QCOMPARE(doc.toPlainText().toUtf8(), original);
        doc.resize(600, 300);
        doc.show();   // (a hidden widget's resize comes when it is shown)
        QCoreApplication::processEvents();
        QVERIFY2(qAbs(note->geometry().right() + 1 + 8 - doc.viewport()->width()) <= 1,
                 qPrintable(QString("%1 %2").arg(note->geometry().right()).arg(doc.viewport()->width())));
        doc.hide();
        doc.appendPlainText("// typed somehow");
        {
            misc::ErrorCapture capture;
            QCOMPARE(doc.save(), -1);
            QVERIFY(capture.errors().join(' ').contains("change it in the library"));
        }
        QCOMPARE(bytes(team + "/VaLib/good.va"), original);
        // Saved as a file of its own: that one is the user's.
        doc.setName(p + "/mine.va");
        QVERIFY(!doc.isReadOnly());
        QVERIFY(doc.libraryOrigin().isEmpty());
        QVERIFY(note->isHidden());
        QCOMPARE(doc.save(), 0);
        QVERIFY(bytes(p + "/mine.va").contains("// typed somehow"));
        QCOMPARE(bytes(team + "/VaLib/good.va"), original);
        // Any other file: as before.
        TextDoc other(&app, p + "/mine.va");
        QVERIFY(other.load());
        QVERIFY(!other.isReadOnly());
    }

    // The project opened: what its schematics use linked (and the Content
    // panel says whose it is); a link no one uses any more taken away.
    void openingTheProjectBringsItUpToDate()
    {
        const QString p = project("p13");
        write(p + "/use.sch", usesLibrary("VaLib"));
        QucsApp app(false);
        MainGuard guard(&app);
        app.openProject(p);
        QCOMPARE(app.lastLibrarySync().made, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY2(app.statusBar()->currentMessage().contains("Library Verilog-A linked from its library: Libraries/VaLib/good.va"),
                 qPrintable(app.statusBar()->currentMessage()));
        ProjectView* view = app.projectView();
        const QModelIndex row = rowOf(view->model(), "Libraries/VaLib/good.va");
        QVERIFY(row.isValid());
        QCOMPARE(row.sibling(row.row(), 1).data().toString(), QString("library VaLib"));
        QVERIFY(row.data(Qt::ToolTipRole).toString().contains(QDir::toNativeSeparators(real(team + "/VaLib/good.va"))));
        // Another file of the project: no note.
        const QModelIndex sch = rowOf(view->model(), "use.sch");
        QVERIFY(sch.isValid());
        QVERIFY(!sch.sibling(sch.row(), 1).data().toString().contains("library"));
        // Open in a tab, saved without the part since (by another program):
        // the tab's part keeps it - until the tab goes.
        QVERIFY(app.gotoPage(p + "/use.sch"));
        write(p + "/use.sch", plain());
        QVERIFY(!app.syncProjectLibraries().changed());
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        for (QucsDoc* doc : app.allDocuments()) doc->setDocChanged(false);
        QVERIFY(app.closeAllFiles());
        QCOMPARE(app.syncProjectLibraries().removed, QStringList{"Libraries/VaLib/good.va"});
        write(p + "/use.sch", usesLibrary("VaLib"));
        QCOMPARE(app.syncProjectLibraries().made, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
        write(p + "/use.sch", plain());
        app.openProject(p);
        QCOMPARE(app.lastLibrarySync().removed, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
        // No project: nothing done.
        QVERIFY(!app.syncProjectLibraries().changed());
    }

    // In Libraries/, which Qucs-S made: taken away with the last folder in it.
    void librariesIsTakenAwayWhenEmpty()
    {
        const QString p = project("p16");
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::sync(p, {}, Mode::Link);
        QVERIFY(QFileInfo::exists(p + "/Libraries/" + projectlibraries::MarkerName));
        write(p + "/use.sch", plain());
        projectlibraries::sync(p, {}, Mode::Link);
        QVERIFY(!QFileInfo::exists(p + "/Libraries"));
        // The user's own Libraries/ folder (no mark): stays, even empty.
        QVERIFY(QDir().mkpath(p + "/Libraries"));
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::sync(p, {}, Mode::Link);
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        write(p + "/use.sch", plain());
        projectlibraries::sync(p, {}, Mode::Link);
        QVERIFY(QFileInfo(p + "/Libraries").isDir());
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib"));
    }

    // Links are relative: the project and the library moved together (a
    // tree of both cloned or moved, another user's home in the same
    // places), the link still leads to the file - before Qucs-S looks again.
    void aLinkHoldsWhenTheTreeMoves()
    {
        const QString tree = dir.filePath("tree");
        QVERIFY(QDir().mkpath(tree + "/libs/VaLib/inc"));
        QVERIFY(QFile::copy(team + "/VaLib.lib", tree + "/libs/VaLib.lib"));
        QVERIFY(QFile::copy(team + "/VaLib/good.va", tree + "/libs/VaLib/good.va"));
        const QString p = tree + "/work/p17_prj";
        write(p + "/use.sch", usesLibrary(tree + "/libs/VaLib"));
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
#ifndef Q_OS_WIN
        char target[4096] = {0};
        const ssize_t n = ::readlink(QFile::encodeName(p + "/Libraries/VaLib/good.va").constData(), target, sizeof(target) - 1);
        QVERIFY(n > 0);
        QCOMPARE(QString::fromLocal8Bit(target, int(n)), QString("../../../../libs/VaLib/good.va"));
#endif
        QVERIFY(QDir(dir.path()).rename("tree", "tree-moved"));
        const QFileInfo moved(dir.filePath("tree-moved/work/p17_prj/Libraries/VaLib/good.va"));
        QVERIFY(moved.exists());
        QCOMPARE(moved.canonicalFilePath(), real(dir.filePath("tree-moved/libs/VaLib/good.va")));
    }

    // A disk without symbolic links (exFAT, some network shares): a copy,
    // with what it includes; kept as it is while links cannot be made (not
    // made again each time); a link in its place once they can.
    void aCopyWhereNoLinkCanBeMade()
    {
        projectlibraries::setLinkMaker([](const QString&, const QString&) { return false; });
        const auto restore = qScopeGuard([] { projectlibraries::setLinkMaker(nullptr); });
        const QString p = project("p18");
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.made, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(!QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        QCOMPARE(bytes(p + "/Libraries/VaLib/good.va"), bytes(team + "/VaLib/good.va"));
        QVERIFY(QFileInfo::exists(p + "/Libraries/VaLib/inc/common.vams"));
        QCOMPARE(projectlibraries::entryOf(p + "/Libraries/VaLib/good.va").library, QString("VaLib"));
        write(p + "/Libraries/VaLib/good.osdi", "its model");
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QVERIFY(QFileInfo::exists(p + "/Libraries/VaLib/good.osdi"));   // (not compiled again)
        projectlibraries::setLinkMaker(nullptr);
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.made, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib/inc"));
    }

    // The folders made in the project's folder itself, before Libraries/:
    // moved there - what they held taken away, made again in Libraries/;
    // one of a library not found here kept as it is.
    void theFoldersOfBeforeAreMoved()
    {
        const QString p = project("p19");
        write(p + "/use.sch", usesLibrary("VaLib"));
        QVERIFY(QDir().mkpath(p + "/VaLib"));
        QVERIFY(QFile::link(real(team + "/VaLib/good.va"), p + "/VaLib/good.va"));
        write(p + "/VaLib/good.osdi", "its model");
        const auto record = [&](const QString& library, const QString& original) {
            return QByteArray("{\"library\": \"") + library.toUtf8() + "\", \"folder\": \"" + QFileInfo(original).absolutePath().toUtf8()
                   + "\", \"created\": true, \"files\": [{\"path\": \"good.va\", \"original\": \"" + original.toUtf8()
                   + "\", \"kind\": \"link\"}]}";
        };
        write(p + "/VaLib/" + projectlibraries::RecordName, record("VaLib", real(team + "/VaLib/good.va")));
        QVERIFY(QDir().mkpath(p + "/Lost"));
        QVERIFY(QFile::link("/nowhere/Lost/good.va", p + "/Lost/good.va"));
        write(p + "/Lost/" + projectlibraries::RecordName, record("Lost", "/nowhere/Lost/good.va"));
        write(p + "/lost.sch", usesLibrary("/nowhere/Lost"));
        const projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.removed, QStringList{"VaLib/good.va"});
        QCOMPARE(r.made, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(!QFileInfo::exists(p + "/VaLib"));
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        QVERIFY(QFileInfo(p + "/Lost/good.va").isSymLink());   // its library not found here: kept
    }

    // A record edited by hand, or come with a project from elsewhere,
    // names files out of its folder - by ".." anywhere in the path, an
    // absolute path, a folder that is a link to one elsewhere: none is taken
    // away, in Libraries/ or in a folder of before (the 4 October hunt's
    // A1: ./../../victim.txt took the project's file away). In its folder,
    // a copy is taken away only while it is the copy Qucs-S made: the
    // record's sum, or - a record of before sums - its original's bytes.
    void aRecordTakesAwayNothingOutsideItsFolder()
    {
        const QString p = project("p40");
        const QString outside = dir.filePath("outside40");
        write(p + "/use.sch", plain());
        write(p + "/victim.txt", "the user's");
        write(p + "/victim.osdi", "theirs too");
        write(p + "/keep/notes.txt", "notes");
        write(p + "/docs/thesis.tex", "thesis");
        write(outside + "/far.txt", "far");
        QVERIFY(QFile::link(outside + "/far.txt", outside + "/farlink"));
        QVERIFY(QDir().mkpath(p + "/Libraries/Evil"));
        QVERIFY(QFile::link(outside, p + "/Libraries/Evil/out"));   // a folder in it leading elsewhere
        const auto item = [](const QString& path, const QString& kind, const QString& original = "/nowhere/Evil/x.va") {
            return QStringLiteral("{\"path\": \"%1\", \"original\": \"%2\", \"kind\": \"%3\"}").arg(path, original, kind);
        };
        const auto record = [](const QString& library, const QStringList& items) {
            return QStringLiteral("{\"library\": \"%1\", \"folder\": \"/nowhere/%1\", \"created\": true, \"files\": [%2]}")
                .arg(library, items.join(", ")).toUtf8();
        };
        write(p + "/Libraries/Evil/" + projectlibraries::RecordName,
              record("Evil", {item("./../../victim.txt", "copy"), item("x/../../../keep/notes.txt", "copy"),
                              item("../../docs/thesis.tex", "include"), item(p + "/docs/thesis.tex", "copy"),
                              item("out/far.txt", "copy"), item("out/farlink", "link"), item("C:/far.txt", "copy"),
                              item(".", "copy"), item("", "copy")}));
        // A folder of before (the project's own): the same.
        QVERIFY(QDir().mkpath(p + "/OldLib"));
        write(p + "/OldLib/" + projectlibraries::RecordName,
              record("OldLib", {item("./../docs/thesis.tex", "copy"), item("sub/../../docs/thesis.tex", "copy"),
                                item("../victim.txt", "copy")}));
        projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(bytes(p + "/victim.txt"), QByteArray("the user's"));
        QCOMPARE(bytes(p + "/victim.osdi"), QByteArray("theirs too"));
        QCOMPARE(bytes(p + "/keep/notes.txt"), QByteArray("notes"));
        QCOMPARE(bytes(p + "/docs/thesis.tex"), QByteArray("thesis"));
        QCOMPARE(bytes(outside + "/far.txt"), QByteArray("far"));
        QVERIFY(QFileInfo(outside + "/farlink").isSymLink());
        QVERIFY(QFileInfo(p + "/Libraries/Evil/out").isSymLink());   // (not Qucs-S's)
        QVERIFY(!QFileInfo::exists(p + "/Libraries/Evil/" + projectlibraries::RecordName));
        QVERIFY(!QFileInfo::exists(p + "/OldLib"));   // (emptied: Qucs-S made it, as its record says)

        // In its folder: the copies it made, and only those.
        const auto sum = [](const QByteArray& b) {
            return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
        };
        const QString c = p + "/Libraries/Copies/";
        write(outside + "/old.va", "// old\n");
        write(outside + "/stale.va", "// version two\n");
        write(c + "made.va", "// made\n");
        write(c + "edited.va", "// edited by the user\n");
        write(c + "old.va", "// old\n");
        write(c + "stale.va", "// version one\n");   // (its size the same)
        write(c + "self.va", "// itself\n");
        write(c + "orphan.va", "// no original\n");
        write(c + "made.osdi", "its model");
        write(c + "edited.osdi", "its model");
        const auto summed = [&](const QString& path, const QByteArray& made) {
            return item(path, "copy").chopped(1) + QStringLiteral(", \"sum\": \"%1\"}").arg(sum(made));
        };
        write(c + projectlibraries::RecordName,
              record("Copies", {summed("made.va", "// made\n"), summed("edited.va", "// as made\n"),
                                item("old.va", "copy", outside + "/old.va"), item("stale.va", "copy", outside + "/stale.va"),
                                item("self.va", "copy", c + "self.va"), item("orphan.va", "copy", "/nowhere/orphan.va")}));
        const projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.removed, (QStringList{"Libraries/Copies/made.va", "Libraries/Copies/old.va"}));
        QVERIFY(!QFileInfo::exists(c + "made.osdi"));
        QCOMPARE(bytes(c + "edited.va"), QByteArray("// edited by the user\n"));
        QCOMPARE(bytes(c + "edited.osdi"), QByteArray("its model"));
        QCOMPARE(bytes(c + "stale.va"), QByteArray("// version one\n"));
        QCOMPARE(bytes(c + "self.va"), QByteArray("// itself\n"));
        QCOMPARE(bytes(c + "orphan.va"), QByteArray("// no original\n"));
        QCOMPARE(bytes(outside + "/old.va"), QByteArray("// old\n"));

        // A copy Qucs-S makes has its sum in the record.
        const QString q = project("p40b");
        write(q + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::sync(q, {}, Mode::Copy);
        const QByteArray kept = bytes(q + "/Libraries/VaLib/" + projectlibraries::RecordName);
        QVERIFY2(kept.contains(sum(bytes(team + "/VaLib/good.va")).toUtf8()) && kept.contains(sum(bytes(team + "/VaLib/inc/common.vams")).toUtf8()),
                 kept.constData());
    }

    // Nothing is written through a folder that leads elsewhere: Libraries/
    // a link (what is there is not the project's: nothing read, written or
    // taken away), a library's folder in it a link, a folder in that one.
    void nothingIsWrittenThroughAFolderLeadingElsewhere()
    {
        const auto sum = [](const QByteArray& b) {
            return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
        };
        // Libraries/ a link: to the Libraries/ of another project.
        const QString theirs = dir.filePath("outside41");
        write(theirs + "/X/x.va", "// theirs\n");
        write(theirs + "/X/" + projectlibraries::RecordName,
              "{\"library\": \"X\", \"folder\": \"/nowhere/X\", \"created\": true, \"files\": [{\"path\": \"x.va\", "
              "\"original\": \"/nowhere/X/x.va\", \"kind\": \"copy\", \"sum\": \"" + sum("// theirs\n").toUtf8() + "\"}]}");
        const QString p = project("p41");
        QVERIFY(QFile::link(theirs, p + "/Libraries"));
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.conflicts, QStringList{"Libraries"});
        QVERIFY(r.made.isEmpty() && r.removed.isEmpty());
        QCOMPARE(bytes(theirs + "/X/x.va"), QByteArray("// theirs\n"));
        QVERIFY(!QFileInfo::exists(theirs + "/VaLib"));

        // A library's folder a link.
        const QString elsewhere = dir.filePath("outside42");
        QVERIFY(QDir().mkpath(elsewhere));
        const QString p2 = project("p42");
        QVERIFY(QDir().mkpath(p2 + "/Libraries"));
        QVERIFY(QFile::link(elsewhere, p2 + "/Libraries/VaLib"));
        write(p2 + "/use.sch", usesLibrary("VaLib"));
        r = projectlibraries::sync(p2, {}, Mode::Link);
        QVERIFY2(r.conflicts.contains("Libraries/VaLib") && r.made.isEmpty(), qPrintable(r.conflicts.join(',')));
        QVERIFY(QDir(elsewhere).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());

        // A folder in it a link (copies: the include goes there) - and the
        // record says the file there is Qucs-S's: neither written nor taken away.
        const QString far = dir.filePath("outside43");
        write(far + "/common.vams", "// the user's\n");
        const QString p3 = project("p43");
        QVERIFY(QDir().mkpath(p3 + "/Libraries/VaLib"));
        QVERIFY(QFile::link(far, p3 + "/Libraries/VaLib/inc"));
        write(p3 + "/Libraries/VaLib/" + projectlibraries::RecordName,
              "{\"library\": \"VaLib\", \"folder\": \"" + real(team + "/VaLib").toUtf8() + "\", \"created\": true, \"files\": ["
              "{\"path\": \"inc/common.vams\", \"original\": \"" + real(team + "/VaLib/inc/common.vams").toUtf8() + "\", \"kind\": \"include\"}]}");
        write(p3 + "/use.sch", usesLibrary("VaLib"));
        r = projectlibraries::sync(p3, {}, Mode::Copy);
        QCOMPARE(r.made, QStringList{"Libraries/VaLib/good.va"});
        QCOMPARE(bytes(far + "/common.vams"), QByteArray("// the user's\n"));
        QCOMPARE(QDir(far).entryList(QDir::Files), QStringList{"common.vams"});
        write(p3 + "/use.sch", plain());
        projectlibraries::sync(p3, {}, Mode::Copy);
        QCOMPARE(bytes(far + "/common.vams"), QByteArray("// the user's\n"));

        // A library whose source is in a folder of its own (va/good.va), that
        // folder in the project a link elsewhere, a file of the user's there
        // the record says is Qucs-S's: neither written over nor linked.
        const QString shelf = dir.filePath("subshelf");
        write(shelf + "/SubLib.lib", bytes(team + "/VaLib.lib").replace("VaLib", "SubLib")
                                         .replace("<SpiceAttach \"good.va\">", "<SpiceAttach \"va/good.va\">"));
        write(shelf + "/SubLib/va/good.va", bytes(team + "/VaLib/good.va"));
        write(shelf + "/SubLib/va/inc/common.vams", bytes(team + "/VaLib/inc/common.vams"));
        QucsSettings.LibraryPaths = {team, shelf};
        const auto restore = qScopeGuard([&] { QucsSettings.LibraryPaths = {team}; });
        const QString beyond = dir.filePath("outside45");
        write(beyond + "/good.va", "// the user's own\n");
        const QString p4 = project("p45");
        QVERIFY(QDir().mkpath(p4 + "/Libraries/SubLib"));
        QVERIFY(QFile::link(beyond, p4 + "/Libraries/SubLib/va"));
        write(p4 + "/Libraries/SubLib/" + projectlibraries::RecordName,
              "{\"library\": \"SubLib\", \"folder\": \"" + real(shelf + "/SubLib").toUtf8() + "\", \"created\": true, \"files\": ["
              "{\"path\": \"va/good.va\", \"original\": \"" + real(shelf + "/SubLib/va/good.va").toUtf8() + "\", \"kind\": \"link\"}]}");
        write(p4 + "/use.sch", usesLibrary("SubLib"));
        for (const Mode mode : {Mode::Link, Mode::Copy}) {
            r = projectlibraries::sync(p4, {}, mode);
            QVERIFY2(r.conflicts == QStringList{"Libraries/SubLib/va/good.va"} && r.made.isEmpty(),
                     qPrintable(r.conflicts.join(',') + " | " + r.made.join(',')));
            QCOMPARE(bytes(beyond + "/good.va"), QByteArray("// the user's own\n"));
            QVERIFY(!QFileInfo(beyond + "/good.va").isSymLink());
            QCOMPARE(QDir(beyond).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{"good.va"});
        }
    }

    // A library's Verilog-A the project keeps is not to be written - its
    // link would write the library's own file (the hunt's A2) - told by the
    // file however its path is spelled; the library's own file, a new file
    // beside it, the record: not refused. A copy: said as a copy. In the
    // window: the save dialogs say it (misc::refusesToWrite()).
    void aKeptFileIsNotToWrite()
    {
        const QString p = project("p44");
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::sync(p, {}, Mode::Link);
        const QString link = p + "/Libraries/VaLib/good.va";
        QVERIFY(QFileInfo(link).isSymLink());
        const QString why = projectlibraries::notToWrite(link);
        QVERIFY2(why.contains("library VaLib") && why.contains("linked into the project") && why.contains(native(real(team + "/VaLib/good.va"))),
                 qPrintable(why));
        QVERIFY(!projectlibraries::notToWrite(p + "/Libraries/VaLib/./good.va").isEmpty());
        const QString alias = dir.filePath("alias44");
        QVERIFY(QFile::link(p, alias));
        QVERIFY(!projectlibraries::notToWrite(alias + "/Libraries/VaLib/good.va").isEmpty());
        if (QFileInfo::exists(p + "/Libraries/VaLib/GOOD.VA"))   // (a disk that ignores case)
            QVERIFY(!projectlibraries::notToWrite(p + "/Libraries/VaLib/GOOD.VA").isEmpty());
        QVERIFY(projectlibraries::notToWrite(team + "/VaLib/good.va").isEmpty());
        QVERIFY(projectlibraries::notToWrite(p + "/Libraries/VaLib/new.txt").isEmpty());
        QVERIFY(projectlibraries::notToWrite(p + "/Libraries/VaLib/" + projectlibraries::RecordName).isEmpty());
        QVERIFY(projectlibraries::notToWrite(p + "/use.sch").isEmpty());
        QVERIFY(projectlibraries::notToWrite(QString()).isEmpty());
        // A link that leads nowhere (the library moved): writing would make
        // the file where the library was - refused too.
        QVERIFY(QFile::remove(link));
        QVERIFY(QFile::link("/nowhere/VaLib/good.va", link));
        QVERIFY(!projectlibraries::notToWrite(link).isEmpty());
        if (QFileInfo::exists(p + "/Libraries/VaLib/GOOD.VA") || QFileInfo(p + "/Libraries/VaLib/GOOD.VA").isSymLink())
            QVERIFY(!projectlibraries::notToWrite(p + "/Libraries/VaLib/GOOD.VA").isEmpty());
        // A copy.
        const QString q = project("p44b");
        write(q + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::sync(q, {}, Mode::Copy);
        const QString copy = projectlibraries::notToWrite(q + "/Libraries/VaLib/good.va");
        QVERIFY2(copy.contains("a copy of the library VaLib's Verilog-A"), qPrintable(copy));
        QVERIFY(!projectlibraries::notToWrite(q + "/Libraries/VaLib/inc/common.vams").isEmpty());
        // The save dialogs' check: said in a message box.
        QString said;
        QTimer::singleShot(0, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                said = box->text();
                box->close();
            }
        });
        QVERIFY(misc::refusesToWrite(nullptr, "Save netlist", alias + "/Libraries/VaLib/good.va"));
        QVERIFY2(said.contains("library VaLib"), qPrintable(said));
        QVERIFY(!misc::refusesToWrite(nullptr, "Save netlist", p + "/netlist.cir"));
    }

    // Two parts of one library, each with its own source: one taken away
    // takes its own link and model, not the other's.
    void twoPartsWithTheirOwnSources()
    {
        const QString p = project("p20");
        write(p + "/use.sch", usesParts("TwoLib", {"sub", "sub2"}));
        projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QStringList made = r.made;
        made.sort();
        QCOMPARE(made, (QStringList{"Libraries/TwoLib/better.va", "Libraries/TwoLib/good.va"}));
        write(p + "/Libraries/TwoLib/better.osdi", "a model");
        write(p + "/Libraries/TwoLib/good.osdi", "a model");
        write(p + "/use.sch", usesParts("TwoLib", {"sub"}));
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.removed, QStringList{"Libraries/TwoLib/better.va"});
        QVERIFY(!QFileInfo::exists(p + "/Libraries/TwoLib/better.osdi"));
        QVERIFY(QFileInfo(p + "/Libraries/TwoLib/good.va").isSymLink());
        QVERIFY(QFileInfo::exists(p + "/Libraries/TwoLib/good.osdi"));
    }

    // A part that names its library by a path not on this computer (placed
    // on another one): the library found by its name, its source linked.
    void aPartFromAnotherComputerFindsItsLibrary()
    {
        const QString p = project("p21");
        write(p + "/use.sch", usesLibrary("/Users/someone/Desktop/VaLib"));
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
        QCOMPARE(real(QFileInfo(p + "/Libraries/VaLib/good.va").symLinkTarget()), real(team + "/VaLib/good.va"));
    }

    // A part names its library by its name when that finds it - a schematic
    // that goes to another computer finds it in its search paths -, else by
    // its path: the Libraries panel's parts and Claude's placements.
    void aPartNamesItsLibraryByNameWhenThatFindsIt()
    {
        QCOMPARE(LibComp::referenceTo(team + "/VaLib.lib"), QString("VaLib"));
        QCOMPARE(LibComp::referenceTo(other + "/VaLib.lib"), QFileInfo(other + "/VaLib").absoluteFilePath());   // the name finds team's
        QucsApp app(false);
        MainGuard guard(&app);
        app.fillLibrariesTreeView();
        QTreeWidget* tree = app.librariesTree();
        QTreeWidgetItem* found = nullptr;   // (the panel's libraries: top-level, after their section's row)
        for (int i = 0; i < tree->topLevelItemCount() && found == nullptr; ++i)
            if (real(tree->topLevelItem(i)->text(1)) == real(team + "/VaLib.lib")) found = tree->topLevelItem(i);
        QVERIFY(found != nullptr);
        QVERIFY(app.readLibraryParts(found));
        QVERIFY(found->childCount() > 0);
        const QString placed = found->child(0)->text(1);
        QVERIFY2(placed.contains("\"VaLib\" 0 \"sub\"") && !placed.contains(team), qPrintable(placed));
        // user_lib's too.
        QVERIFY(QDir().mkpath(workspace + "/user_lib"));
        QVERIFY(QFile::copy(team + "/TwoLib.lib", workspace + "/user_lib/UserOnly.lib"));
        app.fillLibrariesTreeView();
        QTreeWidgetItem* user = nullptr;
        for (int i = 0; i < tree->topLevelItemCount() && user == nullptr; ++i)
            if (tree->topLevelItem(i)->text(1).endsWith("/user_lib/UserOnly.lib")) user = tree->topLevelItem(i);
        QVERIFY(user != nullptr && user->childCount() > 0);
        QVERIFY2(user->child(0)->text(1).contains("\"UserOnly\" 0"), qPrintable(user->child(0)->text(1)));
        QFile::remove(workspace + "/user_lib/UserOnly.lib");
        // A name with a dot: the panel's parser names it shorter - by its path.
        QVERIFY(QFile::copy(team + "/VaLib.lib", team + "/Dotted.Lib.lib"));
        QCOMPARE(LibComp::referenceTo(team + "/Dotted.Lib.lib"), QFileInfo(team + "/Dotted.Lib").absoluteFilePath());
        QFile::remove(team + "/Dotted.Lib.lib");
        // An installed library's, as before.
        QVERIFY(!LibComp::referenceTo(QucsSettings.LibDir + "/nosuch.lib").isEmpty());
    }

    // A library of the name made later that has no such part - the
    // project's own VaLib.lib -: the part stays the search path's, which
    // has it, its pins and its linked source with it. No library of the
    // name has the part: the check says which there are, and the link stays
    // (the part is not loaded, its library not known). The project's own
    // getting the part too: still the one the source was linked from.
    void aLibraryOfTheNameWithoutThePartDoesNotTakeIt()
    {
        const QString p = project("p23");
        write(p + "/use.sch", usesLibrary("VaLib"));
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
        write(p + "/Libraries/VaLib/good.osdi", "a model");
        write(p + "/VaLib.lib", libraryOf("VaLib", "rc"));
        QCOMPARE(LibComp::libraryFileOf("VaLib", p, "sub"), real(team + "/VaLib.lib"));
        QCOMPARE(LibComp::libraryFileOf("VaLib", p, "rc"), real(p + "/VaLib.lib"));
        QCOMPARE(LibComp::libraryFileOf("VaLib", p), real(p + "/VaLib.lib"));   // (no part: the first there is)
        QCOMPARE(LibComp::librariesNamed("VaLib", p), (QStringList{real(p + "/VaLib.lib"), real(team + "/VaLib.lib")}));
        // Named by a path whose library has no such part: the one of its name that has it.
        write(dir.filePath("elsewhere/VaLib.lib"), libraryOf("VaLib", "rc"));
        QCOMPARE(LibComp::libraryFileOf(dir.filePath("elsewhere/VaLib"), p, "sub"), real(team + "/VaLib.lib"));
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        QVERIFY(QFileInfo::exists(p + "/Libraries/VaLib/good.osdi"));
        QucsApp app(false);
        MainGuard guard(&app);
        {
            Schematic sch(nullptr, p + "/use.sch");
            QVERIFY(sch.load());
            LibComp* part = partNamed(sch, "X1");
            QVERIFY(part != nullptr);
            QCOMPARE(part->Ports.size(), 2);
            QCOMPARE(part->libraryFile(), real(team + "/VaLib.lib"));
            QVERIFY2(!issuesOf(sch).contains("VaLib"), qPrintable(issuesOf(sch)));   // (one of them has it)
        }
        // None has the part: which there are; the link kept - and named by
        // the path of one without it.
        write(p + "/use.sch", usesParts("VaLib", {"nosuch"}));
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        write(p + "/use.sch", usesParts(QFileInfo(p + "/VaLib").absoluteFilePath(), {"nosuch"}));
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        write(p + "/use.sch", usesParts("VaLib", {"nosuch"}));
        {
            Schematic sch(nullptr, p + "/use.sch");
            QVERIFY(sch.load());
            const QString issues = issuesOf(sch);
            QVERIFY2(issues.contains("X1: the library part nosuch of VaLib could not be loaded (the libraries VaLib there are - "
                                     + native(real(p + "/VaLib.lib")) + ", " + native(real(team + "/VaLib.lib"))
                                     + " - have no part nosuch): it has no pins"),
                     qPrintable(issues));
            QucsSettings.LibraryPaths.clear();
            const auto restore = qScopeGuard([&] { QucsSettings.LibraryPaths = {team}; });
            QVERIFY2(issuesOf(sch).contains("(the library VaLib there is, " + native(real(p + "/VaLib.lib")) + ", has no part nosuch)"),
                     qPrintable(issuesOf(sch)));
        }
        // One of a later Qucs-S's that has it: said so.
        write(p + "/Later.lib", QByteArray(libraryOf("Later", "sub")).replace(PACKAGE_VERSION, "99.0.0"));
        write(p + "/use.sch", usesParts("Later", {"sub"}));
        {
            Schematic sch(nullptr, p + "/use.sch");
            QVERIFY(sch.load());
            QVERIFY2(issuesOf(sch).contains("X1: the library part sub of Later could not be loaded (it is in " + native(real(p + "/Later.lib"))
                                            + ", which it could not be read from: a library of a later Qucs-S, or damaged)"),
                     qPrintable(issuesOf(sch)));
        }
        write(p + "/use.sch", usesParts("Nowhere", {"sub"}));
        {
            Schematic sch(nullptr, p + "/use.sch");
            QVERIFY(sch.load());
            QVERIFY2(issuesOf(sch).contains("X1: the library part sub of Nowhere could not be loaded (not in the libraries of"),
                     qPrintable(issuesOf(sch)));
        }
        // One saved as UTF-16, with its byte order mark (a Windows editor's
        // "Unicode"): has it, as it loads it.
        const QString marked = QString::fromUtf8(libraryOf("Marked", "sub"));
        write(p + "/Marked.lib", QByteArray("\xFF\xFE", 2)
                                     + QByteArray(reinterpret_cast<const char*>(marked.utf16()), marked.size() * 2));
        QVERIFY(LibComp::hasComponent(p + "/Marked.lib", "sub"));
        write(p + "/marked.sch", usesParts("Marked", {"sub"}));
        {
            Schematic sch(nullptr, p + "/marked.sch");
            QVERIFY(sch.load());
            QCOMPARE(partNamed(sch, "X1")->description(), QString("A resistor"));
            QCOMPARE(partNamed(sch, "X1")->Ports.size(), 2);
        }
        QVERIFY(QFile::remove(p + "/marked.sch"));
        // A file of the name that is no Qucs library, a line of it like the
        // part's: not one that has it.
        write(p + "/VaLib.lib", "* VaLib: SPICE models\n<Component sub>\n.subckt sub a b\nR1 a b 1k\n.ends\n");
        QVERIFY(!LibComp::hasComponent(p + "/VaLib.lib", "sub"));
        QCOMPARE(LibComp::libraryFileOf("VaLib", p, "sub"), real(team + "/VaLib.lib"));
        // The project's own gets the part too: the one linked from keeps it;
        // the record gone, the first.
        QVERIFY(QFile::remove(p + "/VaLib.lib"));
        QVERIFY(QFile::copy(team + "/VaLib.lib", p + "/VaLib.lib"));
        write(p + "/use.sch", usesLibrary("VaLib"));
        QCOMPARE(LibComp::libraryFileOf("VaLib", p, "sub"), real(team + "/VaLib.lib"));
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QVERIFY(QDir(p + "/Libraries").removeRecursively());
        QCOMPARE(LibComp::libraryFileOf("VaLib", p, "sub"), real(p + "/VaLib.lib"));
        // Two parts of one name in a schematic, each found in its own library.
        const QString q = project("p26");
        write(q + "/VaLib.lib", libraryOf("VaLib", "rc"));
        write(q + "/use.sch", usesParts("VaLib", {"rc", "sub"}));
        QCOMPARE(projectlibraries::sync(q, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
    }

    // Two libraries of the name with the part - another search path's put
    // first -: the part stays the one the project linked the source of
    // from (its record), and the check says which is used and which not; by
    // its path, no question. A project with no record of it: the first.
    void theLibraryItWasLinkedFromKeepsThePart()
    {
        const QString p = project("p24");
        write(p + "/use.sch", usesLibrary("VaLib"));
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
        QucsSettings.LibraryPaths = {other, team};
        const auto restore = qScopeGuard([&] { QucsSettings.LibraryPaths = {team}; });
        QCOMPARE(projectlibraries::linkedFolders(p, "VaLib"), QStringList{real(team + "/VaLib")});
        QVERIFY(projectlibraries::linkedFolders(p, "TwoLib").isEmpty());
        QCOMPARE(LibComp::libraryFileOf("VaLib", p, "sub", p), real(team + "/VaLib.lib"));
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QCOMPARE(real(QFileInfo(p + "/Libraries/VaLib/good.va").symLinkTarget()), real(team + "/VaLib/good.va"));
        QucsApp app(false);
        MainGuard guard(&app);
        {
            Schematic sch(nullptr, p + "/use.sch");
            QVERIFY(sch.load());
            QCOMPARE(partNamed(sch, "X1")->libraryFile(), real(team + "/VaLib.lib"));   // (its folder's record: no project open)
            QVERIFY2(issuesOf(sch).contains("X1: more than one library VaLib has a part sub: " + native(real(team + "/VaLib.lib"))
                                            + " is used (the project's Verilog-A of it is linked from there), not "
                                            + native(real(other + "/VaLib.lib"))),
                     qPrintable(issuesOf(sch)));
        }
        write(p + "/path.sch", usesLibrary(QFileInfo(other + "/VaLib").absoluteFilePath()));
        {
            Schematic sch(nullptr, p + "/path.sch");
            QVERIFY(sch.load());
            QCOMPARE(partNamed(sch, "X1")->libraryFile(), real(other + "/VaLib.lib"));
            QVERIFY2(!issuesOf(sch).contains("more than one library"), qPrintable(issuesOf(sch)));
        }
        QVERIFY(QFile::remove(p + "/path.sch"));
        // A schematic in a folder of the project, and one open there: the
        // project's record too.
        write(p + "/models/deep.sch", usesLibrary("VaLib"));
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        {
            Schematic deep(nullptr, p + "/models/deep.sch");
            QVERIFY(deep.load());
            write(p + "/models/deep.sch", plain());
            QVERIFY(!projectlibraries::sync(p, {&deep}, Mode::Link).changed());
        }
        // The record changed by another program: read again. Written again
        // by Qucs-S within the second, on a disk whose times are coarse: too.
        const QString record = p + "/Libraries/VaLib/" + projectlibraries::RecordName;
        const QByteArray was = bytes(record);
        write(record, QByteArray(was).replace(real(team + "/VaLib").toUtf8(), real(other + "/VaLib").toUtf8()));
        setBuilt(record, QDateTime::currentDateTime().addSecs(60));
        QCOMPARE(projectlibraries::linkedFolders(p, "VaLib"), QStringList{real(other + "/VaLib")});
        write(record, was);
        setBuilt(record, QDateTime::currentDateTime().addSecs(120));
        QCOMPARE(projectlibraries::linkedFolders(p, "VaLib"), QStringList{real(team + "/VaLib")});
        const QDateTime before = QFileInfo(record).lastModified();
        QucsSettings.LibraryPaths = {other};
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});   // other's now
        setBuilt(record, before);
        QCOMPARE(projectlibraries::linkedFolders(p, "VaLib"), QStringList{real(other + "/VaLib")});
        QucsSettings.LibraryPaths = {other, team};
        const QString q = project("p25");
        write(q + "/use.sch", usesLibrary("VaLib"));
        QCOMPARE(LibComp::libraryFileOf("VaLib", q, "sub", q), real(other + "/VaLib.lib"));
        QCOMPARE(projectlibraries::sync(q, {}, Mode::Link).made, QStringList{"Libraries/VaLib/good.va"});
        QCOMPARE(real(QFileInfo(q + "/Libraries/VaLib/good.va").symLinkTarget()), real(other + "/VaLib/good.va"));
        {
            Schematic sch(nullptr, q + "/use.sch");
            QVERIFY(sch.load());
            QVERIFY2(issuesOf(sch).contains(native(real(other + "/VaLib.lib")) + " is used (found first), not "
                                            + native(real(team + "/VaLib.lib"))),
                     qPrintable(issuesOf(sch)));
        }
    }

    // A library made with the name of another one there is: said.
    void aLibraryMadeWithAnotherOnesNameSaysSo()
    {
        Module::registerModules();   // (an earlier test's app unregistered them)
        const auto modules = qScopeGuard([] { Module::registerModules(); });   // (and this one's)
        QucsApp app(false);
        MainGuard guard(&app);
        app.openProject(source);
        const auto made = [&](const QString& name) {
            LibraryDialog dialog(&app);
            dialog.fillSchematicList({"sub.sch"});
            LibraryDialog::Request request;
            request.name = name;
            request.subcircuits = {"sub.sch"};
            request.folder = dir.filePath("third");
            QString log, error;
            return dialog.create(request, &log, &error) ? log : QStringLiteral("not made: ") + error + "\n" + log;
        };
        QString log = made("VaLib");
        QVERIFY2(log.contains("Note: another library is named VaLib too: " + native(real(team + "/VaLib.lib")) + "."), qPrintable(log));
        log = made("Unique");
        QVERIFY2(log.contains("Successfully created library") && !log.contains("another library is named"), qPrintable(log));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
    }

    // OpenVAF given a link's file and told where to write; nothing more for
    // a source beside its model.
    void whatOpenVafIsGiven()
    {
        const QString p = project("p22");
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::sync(p, {}, Mode::Link);
        const QString link = p + "/Libraries/VaLib/good.va";
        QCOMPARE(osdi::compileArguments(link),
                 (QStringList{real(team + "/VaLib/good.va"), "-o", QFileInfo(p + "/Libraries/VaLib/good.osdi").absoluteFilePath()}));
        QCOMPARE(osdi::compileArguments(source + "/good.va"), QStringList{QFileInfo(source + "/good.va").absoluteFilePath()});
        QCOMPARE(osdi::compileArguments(source + "/good.va", dir.filePath("cache/good.osdi")),
                 (QStringList{QFileInfo(source + "/good.va").absoluteFilePath(), "-o", dir.filePath("cache/good.osdi")}));
    }

    // Create Library of a subcircuit that places a device of a library the
    // project has linked: its source embedded once, and the files it
    // includes, found beside the original.
    void aLibraryMadeOfItEmbedsTheSourceOnce()
    {
        const QString p = project("p15");
        write(p + "/wrap.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
                               "  <Port P1 1 60 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                               "  <Port P2 1 160 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n"
                               "  <Lib X1 1 110 100 20 -20 0 0 \"VaLib\" 0 \"sub\" 0>\n"
                               "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        QucsApp app(false);
        MainGuard guard(&app);
        app.openProject(p);
        QVERIFY(QFileInfo(p + "/Libraries/VaLib/good.va").isSymLink());
        LibraryDialog dialog(&app);
        dialog.fillSchematicList({"wrap.sch"});
        LibraryDialog::Request request;
        request.name = "Wrapped";
        request.subcircuits = {"wrap.sch"};
        request.folder = dir.filePath("wrapped");
        QString log, error;
        QVERIFY2(dialog.create(request, &log, &error), qPrintable(error + "\n" + log));
        QVERIFY2(log.contains("Embedding Verilog-A: good.va, inc/common.vams (compiled"), qPrintable(log));
        QVERIFY2(!log.contains("Warning"), qPrintable(log));
        QCOMPARE(bytes(dir.filePath("wrapped/Wrapped/good.va")), bytes(team + "/VaLib/good.va"));
        QVERIFY(!QFileInfo(dir.filePath("wrapped/Wrapped/good.va")).isSymLink());
        QVERIFY(QFileInfo::exists(dir.filePath("wrapped/Wrapped/inc/common.vams")));
        QCOMPARE(QString::fromUtf8(bytes(dir.filePath("wrapped/Wrapped.lib"))).count("<SpiceAttach"), 1);
        // The next netlist has the library's part's model (the subcircuits
        // written for the library were taken for written in it).
        Schematic top(nullptr, write(p + "/top.sch", usesLibrary("VaLib")));
        QVERIFY(top.load());
        Ngspice kernel(&top);
        kernel.setWorkdir(dir.filePath("kernel"));
        kernel.SaveNetlist(dir.filePath("kernel/p15.cir"), false);
        const QString netlist = QString::fromUtf8(bytes(dir.filePath("kernel/p15.cir")));
        QVERIFY2(netlist.contains(".SUBCKT VaLib_sub") && netlist.contains(".model m1 good"), qPrintable(netlist));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
    }

    // A simulation of a schematic with a device placed and not saved: its
    // source linked first, OpenVAF compiling the link (its model beside
    // it), the netlist loading that; nothing in the library's folder.
    void aSimulationLinksAndCompilesFirst()
    {
#ifdef Q_OS_WIN
        QSKIP("Shell scripts stand in for OpenVAF and ngspice.");
#endif
        const QString calls = dir.filePath("openvaf-calls.log");
        const QString record = dir.filePath("ngspice-got.cir");
        const QString openvaf = write(dir.filePath("fake-openvaf.sh"), QStringLiteral(
            "#!/bin/sh\n"
            "echo \"$*\" >> \"%1\"\n"
            "out=\"${1%.va}.osdi\"\n"
            "if [ \"$2\" = \"-o\" ]; then out=\"$3\"; fi\n"
            "printf '\\000good\\000' > \"$out\" || exit 65\n").arg(calls).toUtf8());
        const QString ngspice = write(dir.filePath("fake-ngspice.sh"), QStringLiteral(
            "#!/bin/sh\n"
            "for a in \"$@\"; do case \"$a\" in *.cir) cp \"$a\" \"%1\";; esac; done\n"
            "echo \"fake ngspice\"\n").arg(record).toUtf8());
        for (const QString& script : {openvaf, ngspice})
            QVERIFY(QFile::setPermissions(script, QFile::permissions(script) | QFile::ExeOwner));
        const QString savedNgspice = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QucsSettings.OpenVAFExecutable = openvaf;
        const auto restore = qScopeGuard([&] {
            QucsSettings.NgspiceExecutable = savedNgspice;
            QucsSettings.OpenVAFExecutable.clear();
        });

        const QString p = project("p14");
        const QString file = write(p + "/top.sch", usesLibrary("VaLib"));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic sch(nullptr, file);
        QVERIFY(sch.load());
        write(file, plain());   // the part placed, not saved
        app.openProject(p);
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib"));
        QPlainTextEdit console;
        QListWidget log;
        QProgressBar progress;
        {
            SimulationRun run(&sch, false);
            run.attach(&console, &log, &progress);
            QSignalSpy done(&run, &SimulationRun::simulated);
            run.start();
            QVERIFY(done.size() > 0 || done.wait(30000));
            QVERIFY2(!run.hasError(), qPrintable(console.toPlainText()));
        }
        const QString link = QFileInfo(p + "/Libraries/VaLib/good.va").absoluteFilePath();
        QVERIFY(QFileInfo(link).isSymLink());
        QStringList said;
        for (int i = 0; i < log.count(); ++i) said << log.item(i)->text();
        // OpenVAF given the file the link leads to (its `include lines found
        // beside it, whatever an OpenVAF does with links), writing beside the link.
        const QString expected = real(team + "/VaLib/good.va") + " -o " + QFileInfo(p + "/Libraries/VaLib/good.osdi").absoluteFilePath();
        QVERIFY2(QString::fromUtf8(bytes(calls)).trimmed() == expected,
                 qPrintable(QString::fromUtf8(bytes(calls)) + "\n" + console.toPlainText() + "\n" + said.join('\n')
                            + "\n" + QString::fromUtf8(bytes(record))));
        QVERIFY(QFileInfo::exists(p + "/Libraries/VaLib/good.osdi"));
        QVERIFY(!QFileInfo::exists(team + "/VaLib/good.osdi"));
        const QString netlist = QString::fromUtf8(bytes(record));
        QVERIFY2(netlist.contains("pre_osdi '" + QFileInfo(p + "/Libraries/VaLib/good.osdi").absoluteFilePath() + "'"), qPrintable(netlist));
        // The schematic gone (closed without saving): taken away at the next.
        QCOMPARE(app.syncProjectLibraries().removed, QStringList{"Libraries/VaLib/good.va"});
        QVERIFY(!QFileInfo::exists(p + "/Libraries/VaLib"));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
    }

    // A part of a library marked to be loaded in every circuit of a project
    // (its subcircuit's Document Settings > Library): a project that places
    // another part of the library has the marked one's source linked too,
    // and each circuit of it loads that, placed or not - a circuit outside
    // it does not; the library's other parts, not placed, are not linked.
    // A project that places none of the library has neither; one that stops
    // placing it, its links taken away.
    void aMarkedPartIsLinkedAndLoadedInEveryCircuitOfTheProject()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "src";
        QucsSettings.QucsWorkDir.setPath(source);
        write(source + "/always.va", "`include \"disciplines.vams\"\nmodule always(p, n);\nendmodule\n");
        write(source + "/always.sch", QString::fromUtf8(bytes(source + "/sub.sch"))
                                          .replace(".model m1 good", ".model m3 always")
                                          .replace("<Components>", "<Properties>\n  <AlwaysLoadOSDI=1>\n</Properties>\n<Components>")
                                          .toUtf8());
        {
            LibraryDialog dialog(&app);
            dialog.fillSchematicList({"sub.sch", "sub2.sch", "always.sch"});
            LibraryDialog::Request request;
            request.name = "MarkLib";
            request.subcircuits = {"sub.sch", "sub2.sch", "always.sch"};
            request.folder = team;
            QString log, error;
            QVERIFY2(dialog.create(request, &log, &error), qPrintable(error + "\n" + log));
            QVERIFY2(log.contains("Marked: every circuit"), qPrintable(log));
        }
        app.ProjName.clear();
        QCOMPARE(LibComp::alwaysLoaded(team + "/MarkLib.lib"), QStringList{"always"});
        const auto netlistOf = [&](const QString& file) {
            Schematic sch(nullptr, file);
            if (!sch.load()) return QString("(not loaded)");
            Ngspice kernel(&sch);
            kernel.setWorkdir(dir.filePath("kernel"));
            kernel.SaveNetlist(dir.filePath("kernel/p15.cir"), false);
            return QString::fromUtf8(bytes(dir.filePath("kernel/p15.cir")));
        };

        // Placing sub (good.va): always.va linked too; better.va (sub2) not.
        const QString p = project("p15");
        write(p + "/use.sch", usesParts("MarkLib", {"sub"}));
        const QString plainHere = write(p + "/plain.sch", plain());
        app.openProject(p);
        QStringList made = app.lastLibrarySync().made;
        made.sort();
        QCOMPARE(made, (QStringList{"Libraries/MarkLib/always.va", "Libraries/MarkLib/good.va"}));
        QCOMPARE(projectlibraries::alwaysLoadedModules(p), QSet<QString>{"always"});
        // Compiled (as OpenVAF does), beside the links.
        write(p + "/Libraries/MarkLib/always.osdi", QByteArray(64, '\0') + "always" + QByteArray(1, '\0'));
        write(p + "/Libraries/MarkLib/good.osdi", QByteArray(64, '\0') + "good" + QByteArray(1, '\0'));
        const QString always = "pre_osdi '" + QFileInfo(p + "/Libraries/MarkLib/always.osdi").absoluteFilePath() + "'";
        const QString good = "pre_osdi '" + QFileInfo(p + "/Libraries/MarkLib/good.osdi").absoluteFilePath() + "'";
        QString netlist = netlistOf(plainHere);
        QVERIFY2(netlist.contains(always) && !netlist.contains(good), qPrintable(netlist));
        netlist = netlistOf(p + "/use.sch");
        QVERIFY2(netlist.contains(always) && netlist.contains(good), qPrintable(netlist));
        // Its sources compiled for a circuit of it - the marked one too.
        QFile::remove(p + "/Libraries/MarkLib/always.osdi");
        {
            Schematic sch(nullptr, plainHere);
            QVERIFY(sch.load());
            Ngspice kernel(&sch);
            kernel.setWorkdir(dir.filePath("kernel"));
            const QList<osdi::Build> builds = kernel.verilogABuilds();
            QCOMPARE(builds.size(), 1);
            QCOMPARE(builds.first().source, QFileInfo(p + "/Libraries/MarkLib/always.va").absoluteFilePath());
        }
        // A circuit outside the project: not loaded.
        const QString outside = write(dir.filePath("outside/plain.sch"), plain());
        netlist = netlistOf(outside);
        QVERIFY2(!netlist.contains("always"), qPrintable(netlist));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));

        // A project that places none of the library: nothing linked, loaded.
        const QString q = project("p16");
        const QString plainThere = write(q + "/plain.sch", plain());
        app.openProject(q);
        QVERIFY(!QFileInfo::exists(q + "/Libraries/MarkLib"));
        QVERIFY(projectlibraries::alwaysLoadedModules(q).isEmpty());
        netlist = netlistOf(plainThere);
        QVERIFY2(!netlist.contains("always"), qPrintable(netlist));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));

        // p placing it no longer: both taken away.
        write(p + "/use.sch", plain());
        QStringList removed = projectlibraries::sync(p, {}, Mode::Link).removed;
        removed.sort();
        QCOMPARE(removed, (QStringList{"Libraries/MarkLib/always.va", "Libraries/MarkLib/good.va"}));
        QVERIFY(projectlibraries::alwaysLoadedModules(p).isEmpty());
        QFile::remove(team + "/MarkLib.lib");
        QDir(team + "/MarkLib").removeRecursively();
    }
};

QTEST_MAIN(TestProjectLibraries)
#include "test_project_libraries.moc"
