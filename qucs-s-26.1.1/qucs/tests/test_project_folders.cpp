/*
 * Which folders are projects: those named NAME_prj - or, with "Any folder
 * is a project" on (Settings, Locations), any folder: the workspace's are
 * listed and open as projects, Import and Link Project take a folder under
 * the name it has, New Project names it as typed, a folder from the system
 * opens as the project. user_lib and hidden folders never are projects.
 */
#include <QtTest>
#include <QCheckBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>

#include "config.h"
#include "claudecode.h"
#include "filebrowser.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucsdoc.h"
#include "settings.h"
#include "workspace.h"
#include "dialogs/newprojdialog.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::workspace;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The setting on for the test's scope, off again after it.
struct AnyFolder {
    explicit AnyFolder(bool on) { QucsSettings.AnyFolderIsProject = on; }
    ~AnyFolder() { QucsSettings.AnyFolderIsProject = false; }
};

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

// Runs fn and answers the dialog it brings up with answer(dialog).
void answering(const std::function<void()>& fn, const std::function<bool(QWidget*)>& answer)
{
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        if (QWidget* modal = QApplication::activeModalWidget())
            if (answer(modal)) timer.stop();
    });
    timer.start(20);
    fn();
}

// The names the Projects panel lists, in its order.
QStringList listed(QListView* view)
{
    QStringList names;
    const QModelIndex root = view->rootIndex();
    for (int r = 0; r < view->model()->rowCount(root); ++r) names << view->model()->index(r, 0, root).data().toString();
    return names;
}

QModelIndex rowOf(QListView* view, const QString& name)
{
    const QModelIndex root = view->rootIndex();
    for (int r = 0; r < view->model()->rowCount(root); ++r) {
        const QModelIndex index = view->model()->index(r, 0, root);
        if (index.data().toString() == name) return index;
    }
    return {};
}

// What is drawn at the right end of a row of the Projects panel (its last
// 30 pixels, through the middle): a green dot, a grey one, or nothing.
QString dotOf(QListView* view, const QModelIndex& index)
{
    const QImage image = view->viewport()->grab().toImage();
    const qreal dpr = image.devicePixelRatio();
    const QRect row = view->visualRect(index);
    const QColor ground = image.pixelColor(int((row.right() - 40) * dpr), int(row.center().y() * dpr));
    bool green = false, grey = false;
    for (int x = row.right() - 30; x <= row.right(); ++x)
        for (int y = row.center().y() - 2; y <= row.center().y() + 2; ++y) {
            const QColor c = image.pixelColor(int(x * dpr), int(y * dpr));
            if (c.green() > c.red() + 60 && c.green() > c.blue() + 40) green = true;
            const int apart = std::abs(c.red() - ground.red()) + std::abs(c.green() - ground.green()) + std::abs(c.blue() - ground.blue());
            if (apart > 60 && std::abs(c.red() - c.green()) < 16 && std::abs(c.green() - c.blue()) < 16) grey = true;
        }
    return green ? QStringLiteral("green") : grey ? QStringLiteral("grey") : QStringLiteral("none");
}

// Whether a row of the Projects panel has the project's icon.
bool hasProjectIcon(const QModelIndex& index)
{
    const QImage shown = index.data(Qt::DecorationRole).value<QIcon>().pixmap(16, 16).toImage();
    const QImage project = QIcon(":bitmaps/hicolor/128x128/apps/qucs.png").pixmap(16, 16).toImage();
    return !shown.isNull() && shown == project;
}

} // namespace

class TestProjectFolders : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString workspace;   // the workspace of each test
    QString plain;       // a folder elsewhere, not named NAME_prj: "plain"

    void fresh(const QString& tag)
    {
        workspace = dir.filePath(tag + "/workspace");
        plain = dir.filePath(tag + "/elsewhere/plain");
        QVERIFY(QDir().mkpath(workspace));
        QVERIFY(write(plain + "/amp.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n"));
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
        QucsSettings.projsDir.setPath(workspace);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        // (What goes to the trash goes into the settings' folder here, not
        // into the user's trash: QUCS_TRASH_DIR, useIsolatedSettings.)
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        QVERIFY(!QucsSettings.AnyFolderIsProject);   // off unless the user turns it on
        QVERIFY(!_settings::Get().itemDefault<bool>("AnyFolderIsProject"));
    }

    void theRulesByDefault()
    {
        fresh("default");
        QVERIFY(isProjectName("amp_prj"));
        QVERIFY(!isProjectName("amp"));
        QVERIFY(!isProjectName("_prj"));
        QVERIFY(isProjectFolder(workspace + "/amp_prj"));
        QVERIFY(isProjectFolder(dir.filePath("anywhere/amp_prj")));
        QVERIFY(!isProjectFolder(workspace + "/amp"));
        QCOMPARE(projectName("amp_prj"), QStringLiteral("amp"));
        QCOMPARE(folderFor("amp"), QStringLiteral("amp_prj"));
        QCOMPARE(folderFor("amp_prj"), QStringLiteral("amp_prj"));

        // A plain folder does not come in, and what is said names the setting.
        const Result r = check(plain, workspace);
        QCOMPARE(r.status, Result::Invalid);
        QVERIFY2(r.message.contains("_prj") && r.message.contains("Any folder is a project"), qPrintable(r.message));
    }

    void theRulesWithAnyFolder()
    {
        fresh("any");
        AnyFolder on(true);
        QVERIFY(isProjectName("amp"));
        QVERIFY(isProjectName("amp_prj"));
        QVERIFY(isProjectName("My Filters"));
        QVERIFY(!isProjectName(""));
        QVERIFY(!isProjectName(".git"));
        QVERIFY(!isProjectName("user_lib"));
        QVERIFY(!isProjectName("a/b"));

        // Shown as projects: the workspace's folders, and NAME_prj anywhere.
        QVERIFY(isProjectFolder(workspace + "/amp"));
        QVERIFY(isProjectFolder(workspace + "/amp/"));
        QVERIFY(isProjectFolder(workspace + "/amp_prj"));
        QVERIFY(isProjectFolder(dir.filePath("anywhere/amp_prj")));
        QVERIFY(!isProjectFolder(dir.filePath("anywhere/amp")));
        QVERIFY(!isProjectFolder(workspace + "/user_lib"));
        QVERIFY(!isProjectFolder(workspace + "/.hidden"));
        QVERIFY(!isProjectFolder(workspace + "/amp/sub"));
        // The workspace however spelled: macOS' temporary folders are under
        // /var, which is /private/var.
        const QString real = QFileInfo(workspace).canonicalFilePath();
        QVERIFY(isProjectFolder(real + "/amp"));

        QCOMPARE(projectName("amp"), QStringLiteral("amp"));
        QCOMPARE(projectName("amp_prj"), QStringLiteral("amp"));
        QCOMPARE(folderFor("amp"), QStringLiteral("amp"));
        QCOMPARE(folderFor("amp_prj"), QStringLiteral("amp_prj"));

        // Free names keep the manner of the name.
        QVERIFY(QDir().mkpath(workspace + "/amp"));
        QVERIFY(QDir().mkpath(workspace + "/amp_2"));
        QVERIFY(QDir().mkpath(workspace + "/filter_prj"));
        QCOMPARE(freeName(workspace, "amp"), QStringLiteral("amp_3"));
        QCOMPARE(freeName(workspace, "filter_prj"), QStringLiteral("filter_2_prj"));
    }

    // Import and Link take a folder under the name it has; never a hidden
    // one, nor as user_lib.
    void importAndLinkAPlainFolder()
    {
        fresh("bring");
        AnyFolder on(true);
        const Result copied = importProject(plain, workspace);
        QVERIFY2(copied.status == Result::Done, qPrintable(copied.message));
        QCOMPARE(copied.path, QDir(workspace).absoluteFilePath("plain"));
        QVERIFY(QFileInfo::exists(copied.path + "/amp.sch"));

        const Result linked = linkProject(plain, workspace, "plain linked");
        QVERIFY2(linked.status == Result::Done, qPrintable(linked.message));
        QCOMPARE(linked.path, QDir(workspace).absoluteFilePath("plain linked"));
        QVERIFY(isLink(linked.path));

        QCOMPARE(check(plain, workspace).status, Result::Exists);
        QCOMPARE(check(plain, workspace, "user_lib").status, Result::Invalid);
        const QString hidden = dir.filePath("bring/elsewhere/.hidden");
        QVERIFY(QDir().mkpath(hidden));
        const Result refused = check(hidden, workspace);
        QCOMPARE(refused.status, Result::Invalid);
        QVERIFY2(refused.message.contains("hidden"), qPrintable(refused.message));
    }

    // The application with any folder a project: brought in under its own
    // name (another one asked for as typed), listed and opened as a project.
    void theApplication()
    {
        fresh("app");
        AnyFolder on(true);
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));

        const QString copied = app.bringProjectIn(plain, false);
        QCOMPARE(copied, QDir(workspace).absoluteFilePath("plain"));
        QTRY_COMPARE(app.projectsView()->currentIndex().data().toString(), QStringLiteral("plain"));
        QVERIFY(hasProjectIcon(app.projectsView()->currentIndex()));

        QString asked;
        QString linked;
        answering([&] { linked = app.bringProjectIn(plain, true); },
                  [&](QWidget* w) {
                      auto* input = qobject_cast<QInputDialog*>(w);
                      if (input == nullptr) return false;
                      asked = input->textValue();
                      input->setTextValue("plain two");
                      input->accept();
                      return true;
                  });
        QCOMPARE(asked, QStringLiteral("plain_2"));   // no "_prj" taken off, none put on
        QCOMPARE(linked, QDir(workspace).absoluteFilePath("plain two"));
        QVERIFY(isLink(linked));

        // A double click opens it as a project, named as its folder is.
        QTRY_VERIFY(rowOf(app.projectsView(), "plain").isValid());   // (listed as the file system tells)
        emit app.projectsView()->doubleClicked(rowOf(app.projectsView(), "plain"));
        QCOMPARE(app.ProjName, QStringLiteral("plain"));
        QCOMPARE(QucsSettings.QucsWorkDir.absolutePath(), copied);
        const QString title = app.QWidget::windowTitle();   // (QucsApp has a windowTitle of its own)
        QVERIFY2(title.startsWith("Project: plain ("), qPrintable(title));
        // Nothing written into a folder not made for Qucs: its Scratch is in
        // the cache directory, named by it.
        QVERIFY(!QFileInfo::exists(copied + "/Scratch"));
        const QString cache = misc::cacheDir();
        QVERIFY2(misc::scratchDir().startsWith(QDir::toNativeSeparators(cache + "/projects/plain-")), qPrintable(misc::scratchDir()));
        QVERIFY(QFileInfo(misc::scratchDir()).isDir());
        QCOMPARE(QDir::toNativeSeparators(QucsSettings.tempFilesDir.absolutePath()), misc::scratchDir());
        const QString scratch = misc::scratchDir();
        app.slotMenuProjClose();
        QDir(scratch).removeRecursively();

        // New Project: the folder as typed, no "_prj" added.
        answering([&] { QVERIFY(QMetaObject::invokeMethod(&app, "slotButtonProjNew")); },
                  [](QWidget* w) {
                      auto* d = qobject_cast<NewProjDialog*>(w);
                      if (d == nullptr) return false;
                      d->ProjName->setText("fresh");
                      d->OpenProj->setChecked(false);
                      d->accept();
                      return true;
                  });
        QVERIFY(QFileInfo(workspace + "/fresh").isDir());
        QVERIFY(!QFileInfo::exists(workspace + "/fresh_prj"));
        QVERIFY(QFileInfo(workspace + "/fresh/Scratch").isDir());

        // Deleting a linked plain folder removes the link only.
        answering([&] { QVERIFY(app.deleteProject(linked)); },
                  [](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr) return false;
                      box->button(QMessageBox::Yes)->click();
                      return true;
                  });
        QVERIFY(!QFileInfo::exists(linked) && !isLink(linked));
        QVERIFY(QFileInfo::exists(plain + "/amp.sch"));

        // A project made by Qucs-S keeps its Scratch in it.
        app.openProject(workspace + "/fresh");
        QCOMPARE(app.ProjName, QStringLiteral("fresh"));
        QCOMPARE(misc::scratchDir(), QDir::toNativeSeparators(workspace + "/fresh/Scratch"));
        app.slotMenuProjClose();

        // The workspace is not a project, nor the home folder (Open Project
        // starts in the workspace, and Open there without a folder chosen
        // picked it: a Scratch in it, listed as a project) (bug hunt
        // 2026-09-26, E2).
        for (const QString& folder : {workspace, QDir::homePath()}) {
            QString said;
            answering([&] { app.openProject(folder); },
                      [&](QWidget* w) {
                          auto* box = qobject_cast<QMessageBox*>(w);
                          if (box == nullptr) return false;
                          said = box->text();
                          box->accept();
                          return true;
                      });
            QVERIFY2(said.contains(folder == workspace ? "workspace" : "home"), qPrintable(said));
            QVERIFY(app.ProjName.isEmpty());
        }
        QVERIFY(!QFileInfo::exists(workspace + "/Scratch"));
    }

    // Delete Project moves the folder to the trash - it was deleted for
    // good, and with the setting any folder of the workspace could be.
    // The question names the folder and what is in it, and says so of a
    // folder that is a project by the setting alone; a document open from
    // it with unsaved changes stops it (bug hunt 2026-09-26, A3).
    void deletingAProjectAsksAndTrashes()
    {
        fresh("delete");
        AnyFolder any(true);
        QVERIFY(write(workspace + "/Documents/letter.txt", "precious"));
        QVERIFY(write(workspace + "/Documents/sub/more.txt", "more"));
        QVERIFY(write(workspace + "/amp_prj/amp.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n"));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        QString text, details;
        const auto cancel = [&](QWidget* w) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (box == nullptr || box->objectName() != "deleteProject") return false;
            text = box->text();
            details = box->informativeText();
            box->button(QMessageBox::Cancel)->click();
            return true;
        };
        answering([&] { QVERIFY(!app.deleteProject(workspace + "/Documents")); }, cancel);
        QVERIFY2(text.contains("Documents") && text.contains("trash"), qPrintable(text));
        QVERIFY2(details.contains("2 files") && details.contains("not a folder Qucs-S made"), qPrintable(details));
        QVERIFY2(details.contains(QDir::toNativeSeparators(workspace + "/Documents")), qPrintable(details));
        QCOMPARE(read(workspace + "/Documents/letter.txt"), QByteArray("precious"));
        answering([&] { QVERIFY(!app.deleteProject(workspace + "/amp_prj")); }, cancel);
        QVERIFY2(details.contains("1 file,") && !details.contains("not a folder Qucs-S made"), qPrintable(details));
        QVERIFY(QFileInfo::exists(workspace + "/amp_prj/amp.sch"));

        // A document open from it with unsaved changes: refused.
        QVERIFY(app.gotoPage(workspace + "/Documents/letter.txt"));
        const auto letter = [&]() -> QucsDoc* {
            for (QucsDoc* d : app.allDocuments())
                if (d->getDocName().endsWith("/Documents/letter.txt")) return d;
            return nullptr;
        };
        QVERIFY(letter() != nullptr);
        letter()->setDocChanged(true);
        QString said;
        answering([&] { QVERIFY(!app.deleteProject(workspace + "/Documents")); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr) return false;
                      said = box->text();
                      if (QAbstractButton* ok = box->button(QMessageBox::Ok)) ok->click();
                      else box->button(QMessageBox::Cancel)->click();   // (the question: the test fails below)
                      return true;
                  });
        QVERIFY2(said.contains("letter.txt") && said.contains("unsaved"), qPrintable(said));
        QCOMPARE(read(workspace + "/Documents/letter.txt"), QByteArray("precious"));
        letter()->setDocChanged(false);
        // To the trash (here, not the user's): gone from the workspace,
        // kept in the trash; the document open from it closed.
        answering([&] { QVERIFY(app.deleteProject(workspace + "/Documents")); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr || box->objectName() != "deleteProject") return false;
                      details = box->informativeText();
                      box->findChild<QPushButton*>("deleteProjectTrash")->click();
                      return true;
                  });
        QVERIFY2(details.contains("letter.txt"), qPrintable(details));   // it closes
        QVERIFY(!QFileInfo::exists(workspace + "/Documents"));
        QCOMPARE(read(dir.filePath("settings/trash/Documents/letter.txt")), QByteArray("precious"));
        QVERIFY(letter() == nullptr);
        app.closeAllFiles();
    }

    // A folder from the system (the command line, a drop, the Finder)
    // opens as the project; without the setting it is refused, as before.
    void aFolderFromTheSystem()
    {
        fresh("system");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));

        QString said;
        answering([&] { QCOMPARE(app.openFromSystem({plain}), 0); },
                  [&](QWidget* w) {
                      auto* box = qobject_cast<QMessageBox*>(w);
                      if (box == nullptr) return false;
                      said = box->text();
                      box->accept();
                      return true;
                  });
        QVERIFY2(said.contains("_prj"), qPrintable(said));
        QVERIFY(app.ProjName.isEmpty());

        AnyFolder on(true);
        QCOMPARE(app.openFromSystem({plain}), 1);
        QCOMPARE(app.ProjName, QStringLiteral("plain"));
        QCOMPARE(QucsSettings.QucsWorkDir.absolutePath(), QDir(plain).absolutePath());
        app.slotMenuProjClose();
    }

    // Which project is open, at a glance: a green dot at the right end of
    // its row, a grey one on the other projects, none on a folder that is
    // no project or on ".."; the tooltip says which. It follows Open and
    // Close, and a linked project is the one open whether it was opened
    // through the link or by the folder it leads to. A long name ends
    // before the dot.
    void theOpenProjectHasAGreenDot()
    {
        fresh("dots");
        for (const char* name : {"amp_prj", "filter_prj", "notes"}) QVERIFY(QDir().mkpath(workspace + "/" + name));
        const QString far = dir.filePath("dots/elsewhere/far_prj");
        QVERIFY(QDir().mkpath(far));
        QCOMPARE(linkProject(far, workspace).status, Result::Done);
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        QListView* panel = app.projectsView();
        QTRY_COMPARE(listed(panel), QStringList({"amp_prj", "far_prj", "filter_prj", "notes"}));
        panel->resize(320, 240);
        const auto state = [&](const char* name) {
            return rowOf(panel, name).data(QucsFileSystemModel::ProjectStateRole).toInt();
        };
        const auto tip = [&](const char* name) { return rowOf(panel, name).data(Qt::ToolTipRole).toString(); };
        for (const char* name : {"amp_prj", "far_prj", "filter_prj"}) {
            QCOMPARE(state(name), int(QucsFileSystemModel::ClosedProject));
            QCOMPARE(dotOf(panel, rowOf(panel, name)), QStringLiteral("grey"));
            QVERIFY2(tip(name).startsWith("A project, not open"), qPrintable(tip(name)));
        }
        QCOMPARE(state("notes"), int(QucsFileSystemModel::NoProject));
        QCOMPARE(dotOf(panel, rowOf(panel, "notes")), QStringLiteral("none"));
        QVERIFY(!tip("notes").contains("project"));
        QVERIFY2(tip("far_prj").contains("Linked from"), qPrintable(tip("far_prj")));   // as it was

        app.openProject(workspace + "/amp_prj");
        QCOMPARE(state("amp_prj"), int(QucsFileSystemModel::OpenProject));
        QCOMPARE(dotOf(panel, rowOf(panel, "amp_prj")), QStringLiteral("green"));
        QCOMPARE(tip("amp_prj"), QStringLiteral("The project open now"));
        QCOMPARE(state("filter_prj"), int(QucsFileSystemModel::ClosedProject));
        QCOMPARE(dotOf(panel, rowOf(panel, "filter_prj")), QStringLiteral("grey"));
        // Chosen (highlighted), the dot is still there.
        panel->setCurrentIndex(rowOf(panel, "amp_prj"));
        QCOMPARE(dotOf(panel, rowOf(panel, "amp_prj")), QStringLiteral("green"));
        panel->setCurrentIndex(rowOf(panel, "filter_prj"));
        QCOMPARE(dotOf(panel, rowOf(panel, "filter_prj")), QStringLiteral("grey"));

        app.slotMenuProjClose();
        QCOMPARE(state("amp_prj"), int(QucsFileSystemModel::ClosedProject));
        QCOMPARE(dotOf(panel, rowOf(panel, "amp_prj")), QStringLiteral("grey"));

        // A linked project, opened by where it is.
        app.openProject(far);
        QCOMPARE(state("far_prj"), int(QucsFileSystemModel::OpenProject));
        QCOMPARE(dotOf(panel, rowOf(panel, "far_prj")), QStringLiteral("green"));
        QVERIFY2(tip("far_prj").startsWith("The project open now\nLinked from"), qPrintable(tip("far_prj")));
        QCOMPARE(state("amp_prj"), int(QucsFileSystemModel::ClosedProject));
        app.slotMenuProjClose();
        app.openProject(workspace + "/far_prj");   // and through the link
        QCOMPARE(state("far_prj"), int(QucsFileSystemModel::OpenProject));
        app.slotMenuProjClose();

        // ".." in a folder: none.
        QVERIFY(QDir().mkpath(workspace + "/notes/inner_prj"));
        emit panel->doubleClicked(rowOf(panel, "notes"));
        QTRY_COMPARE(listed(panel), QStringList({"..", "inner_prj"}));
        QCOMPARE(state(".."), int(QucsFileSystemModel::NoProject));
        QCOMPARE(dotOf(panel, rowOf(panel, "..")), QStringLiteral("none"));
        QCOMPARE(dotOf(panel, rowOf(panel, "inner_prj")), QStringLiteral("grey"));

        // A long name is cut short before the dot.
        const QString longName = QStringLiteral("a_project_with_a_name_much_too_long_for_the_panel_prj");
        QVERIFY(QDir().mkpath(workspace + "/notes/" + longName));
        QTRY_VERIFY(rowOf(panel, longName).isValid());
        panel->resize(200, 240);
        QCOMPARE(dotOf(panel, rowOf(panel, longName)), QStringLiteral("grey"));
        // The name's last pixel is left of the dot's room.
        const QImage image = panel->viewport()->grab().toImage();
        const QRect row = panel->visualRect(rowOf(panel, longName));
        const QColor ground = image.pixelColor(int((row.right() - 2) * image.devicePixelRatio()), int(row.top() * image.devicePixelRatio()) + 1);
        int lastInk = -1;
        for (int x = row.left(); x <= row.right() - 30; ++x)
            for (int y = row.top() + 2; y < row.bottom() - 1; ++y) {
                const QColor c = image.pixelColor(int(x * image.devicePixelRatio()), int(y * image.devicePixelRatio()));
                if (std::abs(c.lightness() - ground.lightness()) > 80) lastInk = x;
            }
        const QRect dot = QRect(row.right() - 30, row.top(), 31, row.height());
        QVERIFY2(lastInk > 0 && lastInk < dot.left(), qPrintable(QString("%1 %2").arg(lastInk).arg(dot.left())));
    }

    // Above the projects, the File Browser's filter: the projects and
    // folders whose names hold what is typed, whatever its case - ".."
    // always, to go back up. The project chosen stays chosen while it is
    // listed; one it leaves out is no longer the one Open and Delete act
    // on. It stays as the panel goes into a folder and as folders come;
    // cleared, all of them again.
    void aFilterByNameAsTheFileBrowsers()
    {
        fresh("filter");
        for (const char* name : {"amp_prj", "filter_prj", "Amplifiers/opamp_prj", "Amplifiers/bjt_prj", "notes"})
            QVERIFY(QDir().mkpath(workspace + "/" + name));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        QListView* panel = app.projectsView();
        QTRY_COMPARE(listed(panel), QStringList({"amp_prj", "filter_prj", "Amplifiers", "notes"}));
        const auto shownFolder = [panel] {
            return QFileInfo(panel->rootIndex().data(QFileSystemModel::FilePathRole).toString()).canonicalFilePath();
        };
        const QString top = QFileInfo(workspace).canonicalFilePath();
        QCOMPARE(shownFolder(), top);

        auto* box = app.findChild<QLineEdit*>("projectsFilter");
        QVERIFY(box != nullptr);
        auto* browsers = app.findChild<QLineEdit*>("fbFilter");
        QVERIFY(browsers != nullptr);
        QCOMPARE(box->placeholderText(), QStringLiteral("Filter by name"));
        QCOMPARE(box->placeholderText(), browsers->placeholderText());
        QVERIFY(box->isClearButtonEnabled());
        QCOMPARE(box->actions().size(), browsers->actions().size());   // the magnifier
        // On the Projects tab, between its buttons and the list.
        QWidget* page = nullptr;
        for (auto* tabs : app.findChildren<QTabWidget*>())
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i) == "Projects") page = tabs->widget(i);
        QVERIFY(page != nullptr && page->isAncestorOf(box) && page->isAncestorOf(panel));
        int boxAt = -1;
        for (int i = 0; i < page->layout()->count(); ++i)
            if (QLayout* row = page->layout()->itemAt(i)->layout(); row != nullptr && row->indexOf(box) >= 0) boxAt = i;
        QVERIFY(boxAt > 0);   // after the buttons
        QCOMPARE(page->layout()->indexOf(panel), boxAt + 1);

        panel->setCurrentIndex(rowOf(panel, "amp_prj"));
        box->setText("AMP");
        QCOMPARE(listed(panel), QStringList({"amp_prj", "Amplifiers"}));
        QCOMPARE(shownFolder(), top);
        QCOMPARE(panel->currentIndex().data().toString(), QStringLiteral("amp_prj"));   // still chosen
        box->setText(" filter ");
        QCOMPARE(listed(panel), QStringList({"filter_prj"}));
        QVERIFY(!panel->currentIndex().isValid());   // left out: no longer chosen
        QVERIFY(panel->selectionModel()->selectedIndexes().isEmpty());
        // Nor is the project beside it chosen instead (as the view would).
        box->clear();
        panel->setCurrentIndex(rowOf(panel, "filter_prj"));
        box->setText("amp");
        QCOMPARE(listed(panel), QStringList({"amp_prj", "Amplifiers"}));
        QVERIFY2(!panel->currentIndex().isValid(), qPrintable(panel->currentIndex().data().toString()));
        QVERIFY(panel->selectionModel()->selectedIndexes().isEmpty());
        box->setText("nothing like it");
        QCOMPARE(listed(panel), QStringList());
        QCOMPARE(shownFolder(), top);

        // As folders come.
        box->setText("amp");
        QVERIFY(QDir().mkpath(workspace + "/amp2_prj"));
        QVERIFY(QDir().mkpath(workspace + "/other_prj"));
        QTRY_COMPARE(listed(panel), QStringList({"amp2_prj", "amp_prj", "Amplifiers"}));
        // Into a folder: filtered too, ".." kept.
        emit panel->doubleClicked(rowOf(panel, "Amplifiers"));
        QTRY_COMPARE(listed(panel), QStringList({"..", "opamp_prj"}));
        QCOMPARE(shownFolder(), QFileInfo(workspace + "/Amplifiers").canonicalFilePath());
        box->clear();
        QTRY_COMPARE(listed(panel), QStringList({"..", "bjt_prj", "opamp_prj"}));
        emit panel->doubleClicked(rowOf(panel, ".."));
        QTRY_COMPARE(listed(panel), QStringList({"amp2_prj", "amp_prj", "filter_prj", "other_prj", "Amplifiers", "notes"}));
    }

    // The setting in the dialog: on the Locations tab, applied at once (the
    // Projects panel sorts and marks the projects anew), kept, and off by
    // Default Values.
    void theSettingsDialog()
    {
        fresh("dialog");
        QVERIFY(QDir().mkpath(workspace + "/alpha"));
        QVERIFY(QDir().mkpath(workspace + "/zeta_prj"));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.switchWorkspace(workspace));
        QListView* panel = app.projectsView();
        // Projects first: zeta_prj, then the folder that is none.
        QTRY_COMPARE(listed(panel), QStringList({"zeta_prj", "alpha"}));
        QVERIFY(!hasProjectIcon(rowOf(panel, "alpha")));

        QucsSettingsDialog dlg(&app);
        auto* box = dlg.findChild<QCheckBox*>("anyFolderIsProject");
        QVERIFY(box != nullptr);
        QVERIFY(!box->isChecked());
        auto* tabs = dlg.findChild<QTabWidget*>();
        QVERIFY(tabs != nullptr);
        int tab = -1;
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->widget(i)->isAncestorOf(box)) tab = i;
        QVERIFY(tab >= 0);
        QCOMPARE(tabs->tabText(tab), QStringLiteral("Workspace"));

        box->setChecked(true);
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(QucsSettings.AnyFolderIsProject);
        QVERIFY(_settings::Get().item<bool>("AnyFolderIsProject"));
        // Both projects now: by name.
        QTRY_COMPARE(listed(panel), QStringList({"alpha", "zeta_prj"}));
        QVERIFY(hasProjectIcon(rowOf(panel, "alpha")));
        // The file browser's kinds follow.
        QCOMPARE(qucs_s::files::kindOf(QFileInfo(workspace + "/alpha")).glyph, qucs_s::files::Kind::Project);
        QCOMPARE(qucs_s::files::kindOf(QFileInfo(plain)).glyph, qucs_s::files::Kind::Folder);
        // And what Claude Code is told of projects.
        QVERIFY(!qucs_s::claude::qucsSystemPrompt().contains("ends in _prj"));

        // A dialog opened again shows it on.
        {
            QucsSettingsDialog again(&app);
            QVERIFY(again.findChild<QCheckBox*>("anyFolderIsProject")->isChecked());
        }

        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotDefaultValues"));
        QVERIFY(!box->isChecked());
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        QVERIFY(!QucsSettings.AnyFolderIsProject);
        QVERIFY(!_settings::Get().item<bool>("AnyFolderIsProject"));
        QTRY_COMPARE(listed(panel), QStringList({"zeta_prj", "alpha"}));
        QVERIFY(!hasProjectIcon(rowOf(panel, "alpha")));
        QCOMPARE(qucs_s::files::kindOf(QFileInfo(workspace + "/alpha")).glyph, qucs_s::files::Kind::Folder);
        QVERIFY(qucs_s::claude::qucsSystemPrompt().contains("ends in _prj"));
    }
};

QTEST_MAIN(TestProjectFolders)
#include "test_project_folders.moc"
