/*
 * A project's temporary files live in its Scratch folder: created with
 * the project, used by the simulation kernels and the log while the
 * project is open, listed by the Content panel as "Scratch". Without a
 * project (headless runs) the simulator work directory from the settings
 * is used as before.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QStandardPaths>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "projectView.h"
#include "textdoc.h"
#include "simulationconsole.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// Where a kernel created now would write.
class KernelProbe : public Ngspice
{
public:
    using Ngspice::Ngspice;
    QString workdir() const { return a_workdir; }
    void setOutputs(const QStringList& files) { a_output_files = files; }
};

QStringList childrenOf(QStandardItem* parent)
{
    QStringList names;
    for (int i = 0; parent && i < parent->rowCount(); ++i)
        names << parent->child(i, 0)->text();
    return names;
}
} // namespace

class TestProjectScratch : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString workspace, project;

    static void write(const QString& path, const QByteArray& bytes)
    {
        QDir().mkpath(QFileInfo(path).path());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
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
        QucsSettings.ContentTreeView = false;
        QucsSettings.S4Qworkdir = dir.filePath("cache-workdir");
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();

        workspace = dir.filePath("workspace");
        project = workspace + "/scratch_prj";
        QVERIFY(QDir().mkpath(project));
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
        QucsSettings.projsDir.setPath(workspace);
        QucsSettings.QucsWorkDir.setPath(workspace);
        write(project + "/circuit.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n</Components>\n");
        write(project + "/netlist.cir", "* a spice netlist\n.end\n");
    }

    void withoutAProjectTheSettingsWorkDirIsUsed()
    {
        QCOMPARE(misc::scratchDir(), QucsSettings.S4Qworkdir);   // no application (headless)
        QucsApp app(false);
        MainGuard guard(&app);
        QCOMPARE(misc::scratchDir(), QucsSettings.S4Qworkdir);   // no project open
        Schematic sch(nullptr, QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        KernelProbe kernel(&sch);
        QCOMPARE(kernel.workdir(), QucsSettings.S4Qworkdir);
    }

    void openingAProjectSwitchesToItsScratchFolder()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(!QFileInfo::exists(project + "/Scratch"));
        app.openProject(project);
        QCOMPARE(app.ProjName, QString("scratch"));
        QVERIFY(QFileInfo(project + "/Scratch").isDir());               // created on open
        const QString scratch = QDir::toNativeSeparators(project + "/Scratch");
        QCOMPARE(misc::scratchDir(), scratch);                          // the kernels' directory
        QCOMPARE(QDir::toNativeSeparators(QucsSettings.tempFilesDir.absolutePath()), scratch); // logs, qucsator
        // Every schematic simulated gets a folder of its name under
        // Scratch, so the files of one run do not overwrite another's;
        // a new run of the same schematic goes to the same folder.
        Schematic sch(nullptr, project + "/circuit.sch");
        QVERIFY(sch.load());
        KernelProbe kernel(&sch);
        QCOMPARE(kernel.workdir(), QDir::toNativeSeparators(scratch + "/circuit"));
        QVERIFY(QFileInfo(scratch + "/circuit").isDir());   // made with the kernel
        QCOMPARE(misc::scratchDirFor(project + "/circuit.sch"), kernel.workdir());
        QCOMPARE(misc::scratchDirFor(project + "/sub/dir/amp.sch"),
                 QDir::toNativeSeparators(scratch + "/sub/dir/amp"));   // keeps the project's tree
        QCOMPARE(misc::scratchDirFor(dir.filePath("elsewhere/x.sch")),
                 QDir::toNativeSeparators(scratch + "/x"));             // outside the project: the name
        QCOMPARE(misc::scratchDirFor(QString()), QDir::toNativeSeparators(scratch + "/untitled"));
        QCOMPARE(misc::scratchDirFor(project + "/two.dots.sch"), QDir::toNativeSeparators(scratch + "/two.dots"));

        // Closing the project goes back to the cache directory.
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
        QVERIFY(app.ProjName.isEmpty());
        QCOMPARE(misc::scratchDir(), QucsSettings.S4Qworkdir);
        QCOMPARE(misc::scratchDirFor(project + "/circuit.sch"), QucsSettings.S4Qworkdir);   // flat, as headless runs expect
        QCOMPARE(QucsSettings.tempFilesDir.absolutePath(),
                 QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    }

#ifndef Q_OS_WIN
    // A run writes its netlist, raw output and log into the schematic's
    // own folder under Scratch; a second run of the same schematic writes
    // there again. "Show last netlist" and "Show last messages" open the
    // files of the schematic in front - or, with the netlist in front, of
    // the schematic simulated last.
    void aSimulationWritesIntoTheSchematicsOwnFolder()
    {
        const QString fake = dir.filePath("fake-ngspice.sh");
        write(fake, "#!/bin/sh\necho \"fake ngspice: $*\"\nexit 0\n");
        QVERIFY(QFile::setPermissions(fake, QFile::permissions(fake) | QFile::ExeOwner));
        struct SimGuard { QString exe = QucsSettings.NgspiceExecutable; ~SimGuard() { QucsSettings.NgspiceExecutable = exe; } } simGuard;
        QucsSettings.NgspiceExecutable = fake;
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"),
                            project + "/RCL_resonance.sch"));
        QucsApp app(false);
        MainGuard guard(&app);
        app.show();
        app.openProject(project);
        QVERIFY(app.gotoPage(project + "/RCL_resonance.sch", false, false));
        SimulationConsole* console = app.simulationConsole();
        QVERIFY(console != nullptr);
        const QString folder = project + "/Scratch/RCL_resonance";
        QVERIFY(!QFileInfo::exists(folder));

        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 15000);
        QTRY_VERIFY(console->currentRun() == nullptr);
        QVERIFY(QFileInfo::exists(folder + "/spice4qucs.cir"));
        QVERIFY(QFileInfo::exists(folder + "/log.txt"));
        QVERIFY(!QFileInfo::exists(project + "/Scratch/spice4qucs.cir"));   // not in the root any more
        const QDateTime firstRun = QFileInfo(folder + "/log.txt").lastModified();

        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowLastNetlist"));
        TextDoc* netlist = qobject_cast<TextDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(netlist != nullptr);
        QCOMPARE(QDir::fromNativeSeparators(netlist->getDocName()), folder + "/spice4qucs.cir");
        QVERIFY(QMetaObject::invokeMethod(&app, "slotShowLastMsg"));   // the netlist in front: the same schematic
        TextDoc* log = qobject_cast<TextDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(log != nullptr);
        QCOMPARE(QDir::fromNativeSeparators(log->getDocName()), folder + "/log.txt");

        // Again: the same folder, updated.
        QVERIFY(app.gotoPage(project + "/RCL_resonance.sch", false, false));
        QTest::qWait(1100);   // a second, for the modification time
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 15000);
        QTRY_VERIFY(console->currentRun() == nullptr);
        QVERIFY(QFileInfo(folder + "/log.txt").lastModified() > firstRun);
        QCOMPARE(QDir(project + "/Scratch").entryList(QDir::Dirs | QDir::NoDotAndDotDot).filter("RCL"),
                 QStringList({"RCL_resonance"}));   // one folder for the schematic, not one per run
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
        // Leave the project as the next case expects it.
        QVERIFY(QDir(folder).removeRecursively());
        QVERIFY(QFile::remove(project + "/RCL_resonance.sch"));
        QFile::remove(project + "/RCL_resonance.dat");
    }
#endif

    // The raw simulator output is left in place after the conversion to a
    // dataset (it used to be deleted in release builds), so the Scratch
    // category can show it.
    void rawOutputStaysAfterConversion()
    {
        const QString work = dir.filePath("keep");
        QVERIFY(QDir().mkpath(work));
        QByteArray raw = "Title: t\nDate: d\nPlotname: AC Analysis\nFlags: complex\nNo. Variables: 2\nNo. Points: 1\n"
                         "Variables:\n\t0\tfrequency\tfrequency\n\t1\tv(out)\tvoltage\nBinary:\n";
        QDataStream s(&raw, QIODevice::Append);
        s.setByteOrder(QDataStream::LittleEndian);
        s.setFloatingPointPrecision(QDataStream::DoublePrecision);
        for (double v : {1.0, 0.0, 0.5, 0.25}) s << v;
        write(work + "/spice4qucs.ac.plot", raw);

        Module::registerModules();   // a QucsApp's destructor unregisters them
        Schematic sch(nullptr, QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        KernelProbe kernel(&sch);
        kernel.setWorkdir(work);
        kernel.setOutputs({"spice4qucs.ac.plot"});
        kernel.convertToQucsData(work + "/out.dat");
        QVERIFY(QFileInfo::exists(work + "/out.dat"));
        QVERIFY(QFileInfo::exists(work + "/spice4qucs.ac.plot"));
    }

    // The Content panel: everything under Scratch/ is listed as "Scratch",
    // named relative to that folder, and nowhere else.
    void scratchFilesAreListedUnderScratch()
    {
        write(project + "/Scratch/spice4qucs.cir", "* netlist\n");
        write(project + "/Scratch/spice4qucs.ac.plot", "Title: x\n");
        write(project + "/Scratch/logs/log.txt", "ok\n");

        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        QStandardItemModel* m = view->model();
        QCOMPARE(m->item(ProjectView::Scratch, 0)->text(), QString("Scratch"));
        QCOMPARE(ProjectView::Scratch, ProjectView::Others + 1);   // right below Others
        QCOMPARE(childrenOf(m->item(ProjectView::Scratch, 0)),
                 QStringList({"spice4qucs.ac.plot", "spice4qucs.cir", "logs/log.txt"}));
        QCOMPARE(childrenOf(m->item(ProjectView::SPICE, 0)), QStringList({"netlist.cir"}));   // not the Scratch one
        QCOMPARE(childrenOf(m->item(ProjectView::Others, 0)), QStringList());
        QCOMPARE(view->filePath(m->item(ProjectView::Scratch, 0)->child(1, 0)->index()),
                 QString("Scratch/spice4qucs.cir"));                          // the real path, for open/delete
        QCOMPARE(view->selectedFileUrls(), QList<QUrl>());
        view->selectionModel()->select(m->item(ProjectView::Scratch, 0)->child(2, 0)->index(),
                                       QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(view->selectedFileUrls(), QList<QUrl>({QUrl::fromLocalFile(project + "/Scratch/logs/log.txt")}));

        // As sub-trees: the folder rows start below Scratch itself.
        view->setTreeView(true);
        QStandardItem* scratch = m->item(ProjectView::Scratch, 0);
        QCOMPARE(childrenOf(scratch), QStringList({"spice4qucs.ac.plot", "spice4qucs.cir", "logs"}));
        QCOMPARE(childrenOf(scratch->child(2, 0)), QStringList({"log.txt"}));
        QCOMPARE(view->filePath(scratch->child(2, 0)->child(0, 0)->index()), QString("Scratch/logs/log.txt"));
        view->setTreeView(false);
        QucsSettings.ContentTreeView = false;
    }

    // Python scripts and images have categories of their own, between
    // SPICE and Others; an image in Scratch stays under Scratch.
    void pythonAndImagesHaveTheirOwnCategories()
    {
        write(project + "/analyse.py", "print(1)\n");
        write(project + "/tools/helper.pyw", "\n");
        write(project + "/logo.png", "not really a png\n");
        write(project + "/figures/gain.svg", "<svg/>\n");
        write(project + "/Photo.JPEG", "\n");
        write(project + "/notes.pdf", "\n");   // not an image: Others
        write(project + "/Scratch/plot.png", "\n");

        QucsApp app(false);
        MainGuard guard(&app);
        ProjectView* view = app.projectView();
        view->setProjPath(project);
        QStandardItemModel* m = view->model();
        QCOMPARE(ProjectView::Python, ProjectView::SPICE + 1);
        QCOMPARE(ProjectView::Images, ProjectView::Python + 1);
        QCOMPARE(ProjectView::Others, ProjectView::Images + 1);
        QCOMPARE(m->item(ProjectView::Python, 0)->text(), QString("Python"));
        QCOMPARE(m->item(ProjectView::Images, 0)->text(), QString("Images"));
        QCOMPARE(childrenOf(m->item(ProjectView::Python, 0)), QStringList({"analyse.py", "tools/helper.pyw"}));
        QCOMPARE(childrenOf(m->item(ProjectView::Images, 0)), QStringList({"logo.png", "Photo.JPEG", "figures/gain.svg"}));
        QCOMPARE(childrenOf(m->item(ProjectView::Others, 0)), QStringList({"notes.pdf"}));
        QVERIFY(childrenOf(m->item(ProjectView::Scratch, 0)).contains("plot.png"));
        QVERIFY(!childrenOf(m->item(ProjectView::Images, 0)).contains("plot.png"));
        QCOMPARE(view->filePath(m->item(ProjectView::Images, 0)->child(2, 0)->index()), QString("figures/gain.svg"));
        QCOMPARE(app.fileType("py"), QString("Python script"));
        QCOMPARE(app.fileType("svg"), QString("image"));
        for (const QString& f : {"analyse.py", "tools/helper.pyw", "logo.png", "figures/gain.svg", "Photo.JPEG", "notes.pdf", "Scratch/plot.png"})
            QVERIFY(QFile::remove(project + "/" + f));
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestProjectScratch test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_project_scratch.moc"
