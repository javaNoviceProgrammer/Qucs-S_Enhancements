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
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"

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
        Schematic sch(nullptr, project + "/circuit.sch");
        QVERIFY(sch.load());
        KernelProbe kernel(&sch);
        QCOMPARE(kernel.workdir(), scratch);

        // Closing the project goes back to the cache directory.
        QVERIFY(QMetaObject::invokeMethod(&app, "slotMenuProjClose"));
        QVERIFY(app.ProjName.isEmpty());
        QCOMPARE(misc::scratchDir(), QucsSettings.S4Qworkdir);
        QCOMPARE(QucsSettings.tempFilesDir.absolutePath(),
                 QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    }

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
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestProjectScratch test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_project_scratch.moc"
