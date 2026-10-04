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
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
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
    int n = 1;
    for (const QString& lib : libs)
        parts += "  <Lib X" + QByteArray::number(n++) + " 1 " + QByteArray::number(100 * n) + " 100 20 -20 0 0 \""
                 + lib.toUtf8() + "\" 0 \"sub\" 0>\n";
    return "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n" + parts +
           "  <GND * 1 70 100 0 0 0 0>\n"
           "  <.DC DC1 1 500 200 0 40 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 "
           "\"CroutLU\" 0 \"no\" 0>\n"
           "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
}
QByteArray usesLibrary(const QString& lib) { return usesLibraries({lib}); }
QByteArray plain() { return usesLibraries({}); }

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
        QCOMPARE(r.made, QStringList{"VaLib/good.va"});
        QVERIFY(r.removed.isEmpty() && r.conflicts.isEmpty());
        const QFileInfo link(p + "/VaLib/good.va");
        QVERIFY(link.isSymLink());
        QCOMPARE(real(link.symLinkTarget()), real(team + "/VaLib/good.va"));
        QVERIFY(QFileInfo::exists(p + "/VaLib/" + projectlibraries::RecordName));
        QVERIFY(!QFileInfo::exists(p + "/VaLib/inc"));   // its includes: OpenVAF finds them beside the original
        const projectlibraries::Entry e = projectlibraries::entryOf(p + "/VaLib/good.va");
        QCOMPARE(e.library, QString("VaLib"));
        QCOMPARE(real(e.original), real(team + "/VaLib/good.va"));
        QVERIFY(projectlibraries::entryOf(p + "/use.sch").isEmpty());
        QVERIFY(projectlibraries::entryOf(p + "/VaLib/" + projectlibraries::RecordName).isEmpty());
        // Again: as it is.
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QCOMPARE(QDir(team + "/VaLib").entryList(QDir::Files | QDir::Hidden), teamFiles);
        // The project lists it as one of its sources.
        QVERIFY(misc::projectFiles(QDir(p), {"*.va"}).contains("VaLib/good.va"));
    }

    // Kept while a schematic uses it; then taken away with the model
    // compiled beside it, the record, and the folder Qucs-S made.
    void noLongerUsedItIsTakenAwayWithItsModel()
    {
        const QString p = project("p1");
        write(p + "/also.sch", usesLibrary("VaLib"));
        write(p + "/VaLib/good.osdi", "a model");
        write(p + "/use.sch", plain());
        projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QVERIFY(!r.changed());   // also.sch uses it
        QVERIFY(QFileInfo(p + "/VaLib/good.va").isSymLink());
        QFile::remove(p + "/also.sch");
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.removed, QStringList{"VaLib/good.va"});
        QVERIFY(!QFileInfo(p + "/VaLib/good.va").isSymLink() && !QFileInfo::exists(p + "/VaLib/good.va"));
        QVERIFY(!QFileInfo::exists(p + "/VaLib/good.osdi"));
        QVERIFY(!QFileInfo::exists(p + "/VaLib"));
        QVERIFY(QFileInfo::exists(team + "/VaLib/good.va"));   // the original, of course
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
    }

    // A folder of the user's with the library's name: a file of theirs in
    // the way is left as it is (and not taken away later), the folder and
    // what else is in it stay.
    void theUsersFilesAreNeverTouched()
    {
        const QString p = project("p2");
        write(p + "/VaLib/notes.txt", "mine");
        write(p + "/VaLib/good.va", "// my own good.va\n");
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.conflicts, QStringList{"VaLib/good.va"});
        QVERIFY(r.made.isEmpty());
        QCOMPARE(bytes(p + "/VaLib/good.va"), QByteArray("// my own good.va\n"));
        r = projectlibraries::sync(p, {}, Mode::Copy);   // (where copies are made: the same)
        QCOMPARE(r.conflicts, QStringList{"VaLib/good.va"});
        QCOMPARE(bytes(p + "/VaLib/good.va"), QByteArray("// my own good.va\n"));
        QVERIFY(projectlibraries::entryOf(p + "/VaLib/good.va").isEmpty());
        write(p + "/use.sch", plain());
        projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(bytes(p + "/VaLib/good.va"), QByteArray("// my own good.va\n"));
        // Out of the way: linked; taken away again - the folder was the user's.
        QFile::remove(p + "/VaLib/good.va");
        write(p + "/use.sch", usesLibrary("VaLib"));
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.made, QStringList{"VaLib/good.va"});
        // A link of the user's, beside it: theirs.
        QVERIFY(QFile::link(team + "/VaLib.lib", p + "/VaLib/mine.lib"));
        write(p + "/use.sch", plain());
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.removed, QStringList{"VaLib/good.va"});
        QVERIFY(QFileInfo::exists(p + "/VaLib/notes.txt"));
        QVERIFY(QFileInfo(p + "/VaLib/mine.lib").isSymLink());
        QVERIFY(!QFileInfo::exists(p + "/VaLib/" + projectlibraries::RecordName));
        // A link it had made, replaced by a file of the user's: theirs now.
        write(p + "/use.sch", usesLibrary("VaLib"));
        projectlibraries::sync(p, {}, Mode::Link);
        QFile::remove(p + "/VaLib/good.va");
        write(p + "/VaLib/good.va", "// edited\n");
        QVERIFY(projectlibraries::entryOf(p + "/VaLib/good.va").isEmpty());   // (opened as the user's, editable)
        write(p + "/use.sch", plain());
        projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(bytes(p + "/VaLib/good.va"), QByteArray("// edited\n"));
    }

    // A library a part names that is not found here (another computer):
    // what it has is kept.
    void aLibraryNotFoundHereKeepsWhatItHas()
    {
        const QString p = project("p3");
        write(p + "/use.sch", usesLibrary("VaLib"));
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"VaLib/good.va"});
        write(p + "/VaLib/good.osdi", "a model");
        QucsSettings.LibraryPaths.clear();
        const auto restore = qScopeGuard([&] { QucsSettings.LibraryPaths = {team}; });
        const projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QVERIFY(!r.changed());
        QVERIFY(QFileInfo(p + "/VaLib/good.va").isSymLink());
        QVERIFY(QFileInfo::exists(p + "/VaLib/good.osdi"));
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
        QCOMPARE(real(QFileInfo(p + "/VaLib/good.va").symLinkTarget()), real(a + "/VaLib/good.va"));
        write(p + "/VaLib/good.osdi", "compiled from teamA's");
        QucsSettings.LibraryPaths = {b};
        QVERIFY(QDir(a).removeRecursively());   // (a link that leads nowhere now)
        const projectlibraries::Report r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.made, QStringList{"VaLib/good.va"});
        QCOMPARE(real(QFileInfo(p + "/VaLib/good.va").symLinkTarget()), real(b + "/VaLib/good.va"));
        QVERIFY(!QFileInfo::exists(p + "/VaLib/good.osdi"));
        QVERIFY(!QFileInfo::exists(p + "/VaLib_2"));   // the same library's folder
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
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"VaLib/good.va"});

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
        QCOMPARE(projectlibraries::sync(o, {&sch}, Mode::Link).made, QStringList{"VaLib/good.va"});
        QCOMPARE(projectlibraries::sync(o, {}, Mode::Link).removed, QStringList{"VaLib/good.va"});
        // One open placing a subcircuit outside the project that places one.
        const QString file2 = write(o + "/new2.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
                                    "  <Sub SUB1 1 260 130 26 -20 0 1 \"" + outside.toUtf8() + "\" 0>\n"
                                    "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        Schematic sch2(nullptr, file2);
        QVERIFY(sch2.load());
        write(file2, plain());
        QCOMPARE(projectlibraries::sync(o, {&sch2}, Mode::Link).made, QStringList{"VaLib/good.va"});
        QCOMPARE(projectlibraries::sync(o, {}, Mode::Link).removed, QStringList{"VaLib/good.va"});
        // A part whose library is not found: kept as unresolved, no folder.
        QList<projectlibraries::Use> uses;
        QStringList unresolved;
        write(o + "/lost.sch", usesLibrary("/nowhere/Lost"));
        projectlibraries::usedSources(o, {}, &uses, &unresolved);
        QVERIFY(uses.isEmpty());
        QCOMPARE(unresolved, QStringList{"Lost"});
    }

    // The project's own library (NAME.lib and its folder in the project):
    // its files are the project's already - nothing linked; and a library
    // of that name from elsewhere takes another folder.
    void aLibraryInTheProjectIsNotLinked()
    {
        const QString p = project("p8");
        QVERIFY(QDir().mkpath(p + "/VaLib/inc"));
        QVERIFY(QFile::copy(team + "/VaLib.lib", p + "/VaLib.lib"));
        QVERIFY(QFile::copy(team + "/VaLib/good.va", p + "/VaLib/good.va"));
        write(p + "/use.sch", usesLibrary("VaLib"));   // beside the schematic: the project's
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Link).changed());
        QVERIFY(!QFileInfo(p + "/VaLib/good.va").isSymLink());
        write(p + "/use.sch", usesLibrary(team + "/VaLib"));
        QCOMPARE(projectlibraries::sync(p, {}, Mode::Link).made, QStringList{"VaLib_2/good.va"});
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
        QCOMPARE(made, (QStringList{"VaLib/good.va", "VaLib_2/good.va"}));
        const QStringList targets{real(QFileInfo(p + "/VaLib/good.va").symLinkTarget()),
                                  real(QFileInfo(p + "/VaLib_2/good.va").symLinkTarget())};
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
        QVERIFY2(r.made == QStringList{"VaLib/good.va"}, qPrintable(r.made.join(',') + " | " + r.conflicts.join(',')));
        QVERIFY(!QFileInfo(p + "/VaLib/good.va").isSymLink());
        QCOMPARE(bytes(p + "/VaLib/good.va"), bytes(team + "/VaLib/good.va"));
        QCOMPARE(bytes(p + "/VaLib/inc/common.vams"), bytes(team + "/VaLib/inc/common.vams"));
        QCOMPARE(projectlibraries::entryOf(p + "/VaLib/good.va").library, QString("VaLib"));
        QCOMPARE(projectlibraries::entryOf(p + "/VaLib/inc/common.vams").library, QString("VaLib"));
        QVERIFY(!projectlibraries::sync(p, {}, Mode::Copy).changed());
        // The original changed: renewed.
        const QByteArray was = bytes(team + "/VaLib/good.va");
        const auto restore = qScopeGuard([&] { write(team + "/VaLib/good.va", was); });
        write(team + "/VaLib/good.va", was + "// changed\n");
        setBuilt(team + "/VaLib/good.va", QDateTime::currentDateTime().addSecs(60));
        r = projectlibraries::sync(p, {}, Mode::Copy);
        QCOMPARE(r.made, QStringList{"VaLib/good.va"});
        QCOMPARE(bytes(p + "/VaLib/good.va"), was + "// changed\n");
        // Links now: the copy and its include replaced, the include taken away.
        r = projectlibraries::sync(p, {}, Mode::Link);
        QCOMPARE(r.made, QStringList{"VaLib/good.va"});
        QVERIFY(QFileInfo(p + "/VaLib/good.va").isSymLink());
        QVERIFY(!QFileInfo::exists(p + "/VaLib/inc"));
        // Copies again - a file it includes from outside the library's folder
        // is not copied (nothing goes outside the project's folder of it).
        write(team + "/outside.vams", "// beside the library\n");
        write(team + "/VaLib/good.va", was + "`include \"../outside.vams\"\n");
        setBuilt(team + "/VaLib/good.va", QDateTime::currentDateTime().addSecs(90));
        projectlibraries::sync(p, {}, Mode::Copy);
        QVERIFY(QFileInfo::exists(p + "/VaLib/inc/common.vams"));
        QVERIFY(!QFileInfo::exists(p + "/outside.vams") && !QFileInfo::exists(workspace + "/outside.vams"));
        QVERIFY(bytes(p + "/VaLib/good.va").contains("outside.vams"));
        write(p + "/use.sch", plain());
        r = projectlibraries::sync(p, {}, Mode::Copy);
        QCOMPARE(r.removed, QStringList{"VaLib/good.va"});
        QVERIFY(!QFileInfo::exists(p + "/VaLib"));
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
        QCOMPARE(app.lastLibrarySync().made, QStringList{"VaLib/good.va"});
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
            QVERIFY(osdi::modelOf(p + "/VaLib/good.va", misc::cacheDir()).isEmpty());
        }
        QList<osdi::Build> builds = buildsOf(p + "/use.sch");
        QCOMPARE(builds.size(), 1);   // the link alone, not the library's source too
        QCOMPARE(builds.first().source, QFileInfo(p + "/VaLib/good.va").absoluteFilePath());
        QCOMPARE(builds.first().library, QFileInfo(p + "/VaLib/good.osdi").absoluteFilePath());
        QVERIFY(builds.first().into == osdi::Into::Beside);
        // Compiled (as OpenVAF does): loaded from there.
        write(p + "/VaLib/good.osdi", QByteArray(64, '\0') + "good" + QByteArray(1, '\0'));
        Schematic sch(nullptr, p + "/use.sch");
        QVERIFY(sch.load());
        Ngspice kernel(&sch);
        kernel.setWorkdir(dir.filePath("kernel"));
        kernel.SaveNetlist(dir.filePath("kernel/p11.cir"), false);
        const QString netlist = QString::fromUtf8(bytes(dir.filePath("kernel/p11.cir")));
        QVERIFY2(netlist.contains("pre_osdi '" + QFileInfo(p + "/VaLib/good.osdi").absoluteFilePath() + "'"), qPrintable(netlist));
        QVERIFY(buildsOf(p + "/use.sch").isEmpty());
        QVERIFY(!QFileInfo::exists(team + "/VaLib/good.osdi"));
        // One in the library's folder (compiled there before, with no project),
        // built later: the project's is loaded all the same.
        write(team + "/VaLib/good.osdi", QByteArray(64, '\0') + "good" + QByteArray(1, '\0'));
        setBuilt(team + "/VaLib/good.osdi", QDateTime::currentDateTime().addSecs(30));
        kernel.SaveNetlist(dir.filePath("kernel/p11b.cir"), false);
        const QString again = QString::fromUtf8(bytes(dir.filePath("kernel/p11b.cir")));
        QVERIFY2(again.contains("pre_osdi '" + QFileInfo(p + "/VaLib/good.osdi").absoluteFilePath() + "'")
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
        QFile::remove(p + "/VaLib/good.osdi");
        builds = buildsOf(p + "/use.sch");
        QCOMPARE(builds.size(), 1);
        QCOMPARE(builds.first().source, QFileInfo(p + "/VaLib/good.va").absoluteFilePath());
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
        TextDoc doc(&app, p + "/VaLib/good.va");
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
        QCOMPARE(app.lastLibrarySync().made, QStringList{"VaLib/good.va"});
        QVERIFY2(app.statusBar()->currentMessage().contains("Library Verilog-A linked from its library: VaLib/good.va"),
                 qPrintable(app.statusBar()->currentMessage()));
        ProjectView* view = app.projectView();
        const QModelIndex row = rowOf(view->model(), "VaLib/good.va");
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
        QVERIFY(QFileInfo(p + "/VaLib/good.va").isSymLink());
        for (QucsDoc* doc : app.allDocuments()) doc->setDocChanged(false);
        QVERIFY(app.closeAllFiles());
        QCOMPARE(app.syncProjectLibraries().removed, QStringList{"VaLib/good.va"});
        write(p + "/use.sch", usesLibrary("VaLib"));
        QCOMPARE(app.syncProjectLibraries().made, QStringList{"VaLib/good.va"});
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
        write(p + "/use.sch", plain());
        app.openProject(p);
        QCOMPARE(app.lastLibrarySync().removed, QStringList{"VaLib/good.va"});
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
        // No project: nothing done.
        QVERIFY(!app.syncProjectLibraries().changed());
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
        QVERIFY(QFileInfo(p + "/VaLib/good.va").isSymLink());
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
        QVERIFY(!QFileInfo::exists(p + "/VaLib"));
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
        const QString link = QFileInfo(p + "/VaLib/good.va").absoluteFilePath();
        QVERIFY(QFileInfo(link).isSymLink());
        QStringList said;
        for (int i = 0; i < log.count(); ++i) said << log.item(i)->text();
        QVERIFY2(QString::fromUtf8(bytes(calls)).trimmed() == link,
                 qPrintable(QString::fromUtf8(bytes(calls)) + "\n" + console.toPlainText() + "\n" + said.join('\n')
                            + "\n" + QString::fromUtf8(bytes(record))));
        QVERIFY(QFileInfo::exists(p + "/VaLib/good.osdi"));
        QVERIFY(!QFileInfo::exists(team + "/VaLib/good.osdi"));
        const QString netlist = QString::fromUtf8(bytes(record));
        QVERIFY2(netlist.contains("pre_osdi '" + QFileInfo(p + "/VaLib/good.osdi").absoluteFilePath() + "'"), qPrintable(netlist));
        // The schematic gone (closed without saving): taken away at the next.
        QCOMPARE(app.syncProjectLibraries().removed, QStringList{"VaLib/good.va"});
        QVERIFY(!QFileInfo::exists(p + "/VaLib"));
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
    }
};

QTEST_MAIN(TestProjectLibraries)
#include "test_project_libraries.moc"
