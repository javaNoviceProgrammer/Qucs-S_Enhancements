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
#ifdef Q_OS_LINUX
        // The trash in here, not the user's (Qt makes $XDG_DATA_HOME/Trash,
        // not $XDG_DATA_HOME).
        qputenv("XDG_DATA_HOME", QFile::encodeName(dir.filePath("xdg")));
        QVERIFY(QDir().mkpath(dir.filePath("xdg")));
#endif
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
        QVERIFY(QFileInfo(copied + "/Scratch").isDir());
        app.slotMenuProjClose();

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
#ifdef Q_OS_LINUX
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
        QCOMPARE(read(dir.filePath("xdg/Trash/files/Documents/letter.txt")), QByteArray("precious"));
        QVERIFY(letter() == nullptr);
#endif
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
        QCOMPARE(tabs->tabText(tab), QStringLiteral("Locations"));

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
