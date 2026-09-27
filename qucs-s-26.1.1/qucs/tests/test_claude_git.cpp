/*
 * The Claude Code dock's git bar: the repository a folder is in (its
 * branch, the lines changed since where the branch left the default one,
 * untracked files counted), shown above the prompt when the project (or
 * the conversation's folder) is in one - Create PR and the rest asked of
 * Claude, the changes shown file by file, ✕ and ⋯ > Show Git Status.
 */
#include <QtTest>
#include <QAction>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>

#include "config.h"
#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "claudegitbar.h"
#include "gitstatus.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "settings.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::git;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

bool write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

// git in \a dir; its output, or a null string when it failed.
QString gitIn(const QString& dir, const QStringList& args)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(program(), args);
    if (!p.waitForFinished(20000) || p.exitCode() != 0) {
        qWarning() << "git" << args << "failed:" << p.readAllStandardError();
        return QString();
    }
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed() + QLatin1String("");
}

QString canonical(const QString& path)
{
    return QFileInfo(path).canonicalFilePath();
}

QAction* actionNamed(QMenu* menu, const QString& name)
{
    emit menu->aboutToShow();   // filled as it opens
    for (QAction* a : menu->actions())
        if (a->objectName() == name) return a;
    return nullptr;
}

// Waits for the bar to have looked at the repository (and GitHub).
bool settled(ClaudeGitBar* bar)
{
    QSignalSpy updated(bar, &ClaudeGitBar::updated);
    for (int i = 0; i < 200 && (bar->isLooking() || updated.isEmpty()); ++i) {
        if (!bar->isLooking() && updated.isEmpty()) bar->refresh();
        updated.wait(50);
    }
    return !bar->isLooking();
}

} // namespace

class TestClaudeGit : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString repo;    // "amp repo": main, a.txt changed, new.txt and blob.bin untracked
    QString plain;   // a folder in no repository

    bool makeRepo(const QString& path)
    {
        if (!QDir().mkpath(path)) return false;
        if (gitIn(path, {"init", "-q"}).isNull()) return false;
        if (gitIn(path, {"symbolic-ref", "HEAD", "refs/heads/main"}).isNull()) return false;
        if (!write(path + "/a.txt", "line 1\nline 2\nline 3\n")) return false;
        return !gitIn(path, {"add", "a.txt"}).isNull() && !gitIn(path, {"commit", "-q", "-m", "first"}).isNull();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        qputenv("QUCS_CLAUDE", "/nonexistent/claude");   // never the real one
        qputenv("QUCS_GH", "/nonexistent/gh");           // nor GitHub
        if (program().isEmpty()) QSKIP("git is not installed");
        // The tests' repositories alone: none found above them, and none of
        // the user's git settings (signed commits, hooks...).
        qputenv("GIT_CEILING_DIRECTORIES", QFile::encodeName(dir.path()));
        QVERIFY(write(dir.filePath("gitconfig"), "[user]\n\tname = Tester\n\temail = tester@example.com\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(dir.filePath("gitconfig")));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");   // (no dialog about a missing one)
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();

        repo = dir.filePath("amp repo");
        QVERIFY(makeRepo(repo));
        QVERIFY(write(repo + "/a.txt", "line 1\nchanged\nline 3\nline 4\n"));   // +2 -1
        QVERIFY(write(repo + "/new.txt", "line 1\nline 2\nline 3"));              // 3 lines, no last newline
        QVERIFY(write(repo + "/blob.bin", QByteArray("\x01\x00\x02", 3)));
        QVERIFY(QDir().mkpath(repo + "/sub"));
        plain = dir.filePath("plain");
        QVERIFY(QDir().mkpath(plain));
    }

    void theStatusOfARepository()
    {
        const Status s = status(repo + "/sub");   // from a folder inside it
        QVERIFY(s.repository);
        QCOMPARE(canonical(s.root), canonical(repo));
        QCOMPARE(s.branch, QStringLiteral("main"));
        QVERIFY(!s.head.isEmpty());
        QCOMPARE(s.defaultBranch, QStringLiteral("main"));
        QVERIFY(s.onDefaultBranch());
        QVERIFY(!s.hasRemote);
        QVERIFY(s.upstream.isEmpty());
        QCOMPARE(s.base, QStringLiteral("HEAD"));
        QVERIFY(s.dirty);
        QCOMPARE(s.changes.size(), 3);
        QCOMPARE(s.changes.at(0).path, QStringLiteral("a.txt"));
        QCOMPARE(s.changes.at(0).added, 2);
        QCOMPARE(s.changes.at(0).removed, 1);
        QVERIFY(!s.changes.at(0).untracked);
        QCOMPARE(s.changes.at(1).path, QStringLiteral("blob.bin"));
        QVERIFY(s.changes.at(1).binary && s.changes.at(1).untracked);
        QCOMPARE(s.changes.at(2).path, QStringLiteral("new.txt"));
        QCOMPARE(s.changes.at(2).added, 3);
        QVERIFY(s.changes.at(2).untracked);
        QCOMPARE(s.added(), 5);
        QCOMPARE(s.removed(), 1);

        const QString one = diff(s, "a.txt");
        QVERIFY2(one.contains("+changed") && one.contains("-line 2") && !one.contains("new.txt"), qPrintable(one));
        const QString added = diff(s, "new.txt");
        QVERIFY2(added.contains("+++ b/new.txt") && added.contains("@@ -0,0 +1,3 @@") && added.contains("+line 3"),
                 qPrintable(added));
        const QString all = diff(s);
        QVERIFY(all.contains("+changed") && all.contains("+++ b/new.txt") && all.contains("Binary file blob.bin"));

        QCOMPARE(statText(2096, 3), QStringLiteral("+2,096 −3"));
    }

    // A repository's own configuration names programs git would run for
    // what the bar asks - a file system monitor, filters (one of them
    // required, in a file the configuration includes), a text conversion -
    // and gh's git: none runs, and the changes are counted as ever. The
    // user's own filter (the global configuration's, as git-lfs's is)
    // still runs. Opening a downloaded project ran its programs (bug hunt
    // 2026-09-26, B1).
    void theRepositorysProgramsAreNotRun()
    {
#ifdef Q_OS_WIN
        QSKIP("the programs here are shell scripts");
#endif
        const QString evil = dir.filePath("evil repo");
        const QString marks = dir.filePath("marks");
        QVERIFY(QDir().mkpath(marks));
        const auto makeProgram = [&](const QString& name, const QByteArray& then) {
            const QString path = dir.filePath("programs/" + name + ".sh");
            write(path, "#!/bin/sh\necho ran >> '" + QFile::encodeName(marks) + "/" + name.toUtf8() + "'\n" + then);
            QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
            return path;
        };
        const auto ran = [&] { return QDir(marks).entryList(QDir::Files); };
        const auto forget = [&] {
            for (const QString& m : ran()) QFile::remove(marks + "/" + m);
        };
        const QString global = dir.filePath("gitconfig");
        QFile original(global);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray globalBefore = original.readAll();
        original.close();
        QVERIFY(write(global, globalBefore + "[filter \"mine\"]\n\tclean = " + QFile::encodeName(makeProgram("user", "cat\n")) + "\n"));

        QVERIFY(makeRepo(evil));
        QVERIFY(write(evil + "/.gitattributes", "a.txt diff=tc filter=fl\nb.txt filter=pr\nu.txt filter=mine\n"));
        QVERIFY(write(evil + "/b.txt", "b\n"));
        QVERIFY(write(evil + "/u.txt", "u\n"));
        QVERIFY(!gitIn(evil, {"add", "-A"}).isNull());
        QVERIFY(!gitIn(evil, {"commit", "-q", "-m", "attributes"}).isNull());
        QVERIFY(write(evil + "/.git/config",
                      "[core]\n\trepositoryformatversion = 0\n\tbare = false\n\tfsmonitor = " + QFile::encodeName(makeProgram("fsmonitor", "exit 1\n"))
                          + "\n[diff \"tc\"]\n\ttextconv = " + QFile::encodeName(makeProgram("textconv", "cat \"$1\"\n"))
                          + "\n[filter \"fl\"]\n\tclean = " + QFile::encodeName(makeProgram("clean", "cat\n"))
                          + "\n\tsmudge = " + QFile::encodeName(makeProgram("smudge", "cat\n")) + "\n[include]\n\tpath = more.config\n"));
        QVERIFY(write(evil + "/.git/more.config",
                      "[filter \"pr\"]\n\tprocess = " + QFile::encodeName(makeProgram("process", "exit 1\n")) + "\n\trequired = true\n"));
        QVERIFY(write(evil + "/a.txt", "line 1\nchanged\nline 3\n"));   // +1 -1
        QVERIFY(write(evil + "/b.txt", "b\nb\n"));
        QVERIFY(write(evil + "/u.txt", "u\nu\n"));

        // As the repository has it, git runs them.
        QProcess plainGit;
        plainGit.setWorkingDirectory(evil);
        plainGit.start(program(), {"diff", "--numstat", "HEAD", "--"});
        QVERIFY(plainGit.waitForFinished(20000));
        QVERIFY2(ran().contains("fsmonitor") && ran().contains("clean"), qPrintable(ran().join(' ')));
        forget();

        const Status s = status(evil);
        QVERIFY(s.repository);
        QCOMPARE(ran(), QStringList{"user"});   // the user's filter alone
        forget();
        const auto change = [&](const QString& path) {
            for (const FileChange& f : s.changes)
                if (f.path == path) return f;
            return FileChange();
        };
        QCOMPARE(change("a.txt").added, 1);
        QCOMPARE(change("a.txt").removed, 1);
        QCOMPARE(change("b.txt").added, 1);
        QCOMPARE(change("u.txt").added, 1);
        QVERIFY(s.dirty);
        const QString text = diff(s, "a.txt");
        QVERIFY2(text.contains("+changed"), qPrintable(text));
        diff(s);
        QVERIFY2(!ran().contains("fsmonitor") && !ran().contains("textconv") && !ran().contains("clean")
                     && !ran().contains("smudge") && !ran().contains("process"),
                 qPrintable(ran().join(' ')));
        forget();

        // gh asks git too: its git is guarded as well.
        const QString gh = dir.filePath("programs/gh");
        QVERIFY(write(gh, "#!/bin/sh\n\"" + QFile::encodeName(program()) + "\" diff --numstat HEAD -- >/dev/null 2>&1\n"
                          "echo '{\"number\": 7, \"url\": \"https://example.org/7\", \"title\": \"t\", \"state\": \"OPEN\", \"isDraft\": false}'\n"));
        QFile::setPermissions(gh, QFile::permissions(gh) | QFileDevice::ExeOwner);
        qputenv("QUCS_GH", QFile::encodeName(gh));
        const PullRequest pr = pullRequest(s.root, "main");
        qputenv("QUCS_GH", "/nonexistent/gh");
        QCOMPARE(pr.number, 7);
        QVERIFY2(!ran().contains("fsmonitor") && !ran().contains("clean") && !ran().contains("process"), qPrintable(ran().join(' ')));

        QVERIFY(write(global, globalBefore));
    }

    void noRepository()
    {
        QVERIFY(!status(plain).repository);
        QVERIFY(!status(QString()).repository);
        QVERIFY(!status(dir.filePath("missing")).repository);
    }

    // On a branch, the changes a pull request would propose: from where it
    // left the remote's default branch, committed or not; on the default
    // branch, the commits not pushed.
    void aBranchCountsFromTheDefaultOne()
    {
        const QString r = dir.filePath("branches");
        QVERIFY(makeRepo(r));
        QVERIFY(!gitIn(r, {"remote", "add", "origin", dir.filePath("nowhere.git")}).isNull());
        QVERIFY(!gitIn(r, {"update-ref", "refs/remotes/origin/main", "HEAD"}).isNull());
        QVERIFY(!gitIn(r, {"symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/main"}).isNull());
        QVERIFY(!gitIn(r, {"checkout", "-q", "-b", "feature"}).isNull());
        QVERIFY(write(r + "/f.txt", "feature\n"));
        QVERIFY(!gitIn(r, {"add", "f.txt"}).isNull());
        QVERIFY(!gitIn(r, {"commit", "-q", "-m", "feature"}).isNull());
        QVERIFY(write(r + "/a.txt", "line 1\nline 2\nline 3\nline 4\n"));   // not committed

        Status s = status(r);
        QCOMPARE(s.branch, QStringLiteral("feature"));
        QCOMPARE(s.defaultBranch, QStringLiteral("main"));
        QVERIFY(s.hasRemote);
        QCOMPARE(s.base, QStringLiteral("origin/main"));
        QCOMPARE(s.changes.size(), 2);
        QCOMPARE(s.added(), 2);
        QVERIFY(s.dirty);
        QVERIFY(s.upstream.isEmpty());

        // Pushed once, then a commit more: one ahead of its upstream.
        QVERIFY(!gitIn(r, {"update-ref", "refs/remotes/origin/feature", "HEAD"}).isNull());
        QVERIFY(!gitIn(r, {"branch", "-q", "--set-upstream-to=origin/feature"}).isNull());
        QVERIFY(!gitIn(r, {"commit", "-q", "-am", "more"}).isNull());
        s = status(r);
        QCOMPARE(s.upstream, QStringLiteral("origin/feature"));
        QCOMPARE(s.ahead, 1);
        QCOMPARE(s.behind, 0);
        QVERIFY(!s.dirty);
        QCOMPARE(s.base, QStringLiteral("origin/main"));   // still: the whole branch

        // The default branch with a commit not pushed.
        QVERIFY(!gitIn(r, {"checkout", "-q", "main"}).isNull());
        QVERIFY(!gitIn(r, {"branch", "-q", "--set-upstream-to=origin/main"}).isNull());
        QVERIFY(write(r + "/m.txt", "m\n"));
        QVERIFY(!gitIn(r, {"add", "m.txt"}).isNull());
        QVERIFY(!gitIn(r, {"commit", "-q", "-m", "local"}).isNull());
        s = status(r);
        QVERIFY(s.onDefaultBranch());
        QCOMPARE(s.base, QStringLiteral("origin/main"));
        QCOMPARE(s.ahead, 1);
        QCOMPARE(s.changes.size(), 1);
        QCOMPARE(s.changes.at(0).path, QStringLiteral("m.txt"));

        // Detached.
        QVERIFY(!gitIn(r, {"checkout", "-q", "--detach"}).isNull());
        s = status(r);
        QVERIFY(s.repository && s.branch.isEmpty() && !s.head.isEmpty());
    }

    void theBar()
    {
        ClaudeGitBar bar;
        bar.setDirectory(repo);
        QVERIFY(settled(&bar));
        QVERIFY(bar.status().repository);
        QVERIFY(!bar.isHidden());
        QCOMPARE(bar.folderLabel()->text(), QStringLiteral("amp repo"));
        QCOMPARE(bar.branchLabel()->text(), QStringLiteral("main"));
        QVERIFY(!bar.statLabel()->isHidden());
        QVERIFY2(bar.statLabel()->text().contains("+5") && bar.statLabel()->text().contains("−1"),
                 qPrintable(bar.statLabel()->text()));
        QCOMPARE(bar.prButton()->text(), QStringLiteral("Create PR"));

        // Create PR: asked of Claude, about this repository and branch.
        QSignalSpy asked(&bar, &ClaudeGitBar::promptRequested);
        bar.prButton()->click();
        QCOMPARE(asked.size(), 1);
        const QString prompt = asked.takeFirst().at(0).toString();
        QVERIFY2(prompt.contains(QDir::toNativeSeparators(bar.status().root)) && prompt.contains("main")
                     && prompt.contains("default branch") && prompt.contains("pull request")
                     && prompt.contains("no remote"),
                 qPrintable(prompt));
        // Not while Claude is busy.
        bar.setBusy(true);
        bar.prButton()->click();
        QVERIFY(asked.isEmpty());
        QAction* commit = actionNamed(bar.prMenu(), "claudeGitCommit");
        QVERIFY(commit != nullptr && !commit->isEnabled());
        bar.setBusy(false);
        commit = actionNamed(bar.prMenu(), "claudeGitCommit");
        QVERIFY(commit->isEnabled());
        commit->trigger();
        QCOMPARE(asked.size(), 1);
        QVERIFY(asked.takeFirst().at(0).toString().startsWith("Commit the uncommitted changes"));
        QVERIFY(!actionNamed(bar.prMenu(), "claudeGitPush")->isEnabled());   // no remote
        QAction* draft = actionNamed(bar.prMenu(), "claudeGitCreateDraft");
        draft->trigger();
        QVERIFY(asked.takeFirst().at(0).toString().contains("draft"));

        // The changes, file by file.
        bar.showChanges();
        auto* changes = qobject_cast<GitChangesDialog*>(bar.changesDialog());
        QVERIFY(changes != nullptr);
        QCOMPARE(changes->files()->count(), 4);   // all, and the three files
        QVERIFY(changes->diffView()->toPlainText().contains("+changed"));
        changes->files()->setCurrentRow(3);
        QVERIFY(changes->files()->currentItem()->text().startsWith("new.txt"));
        QVERIFY2(changes->diffView()->toPlainText().contains("+line 1")
                     && !changes->diffView()->toPlainText().contains("+changed"),
                 qPrintable(changes->diffView()->toPlainText()));
        QSignalSpy open(&bar, &ClaudeGitBar::openFileRequested);
        emit changes->files()->itemDoubleClicked(changes->files()->item(1));
        QCOMPARE(open.size(), 1);
        QCOMPARE(canonical(open.takeFirst().at(0).toString()), canonical(repo + "/a.txt"));
        changes->close();
        QTRY_VERIFY(bar.changesDialog() == nullptr);

        // ✕: away until the branch changes.
        bar.closeButton()->click();
        QVERIFY(bar.isHidden() && bar.isDismissed());
        bar.refresh();
        QVERIFY(settled(&bar));
        QVERIFY(bar.isHidden());
        QVERIFY(!gitIn(repo, {"checkout", "-q", "-b", "other"}).isNull());
        bar.refresh();
        QTRY_COMPARE(bar.branchLabel()->text(), QStringLiteral("other"));
        QVERIFY(!bar.isHidden());

        // ⋯ > Show Git Status off: no bar; kept.
        ClaudeGitBar::setOn(false);
        QVERIFY(bar.isHidden());
        QVERIFY(!QucsSettingsFile().value("ClaudeCode/gitStatus").toBool());
        ClaudeGitBar::setOn(true);
        QVERIFY(settled(&bar));
        QVERIFY(!bar.isHidden());
        QVERIFY(!gitIn(repo, {"checkout", "-q", "main"}).isNull());
        QVERIFY(!gitIn(repo, {"branch", "-q", "-D", "other"}).isNull());

        // No repository: no bar.
        bar.setDirectory(plain);
        QVERIFY(settled(&bar));
        QVERIFY(bar.isHidden());
    }

    // Nothing changed: nothing to propose.
    void aCleanRepository()
    {
        const QString r = dir.filePath("clean");
        QVERIFY(makeRepo(r));
        ClaudeGitBar bar;
        bar.setDirectory(r);
        QVERIFY(settled(&bar));
        QVERIFY(!bar.isHidden());
        QVERIFY(bar.statLabel()->isHidden());
        QSignalSpy asked(&bar, &ClaudeGitBar::promptRequested);
        bar.prButton()->click();
        QVERIFY(asked.isEmpty());
        QVERIFY(!actionNamed(bar.prMenu(), "claudeGitCreatePr")->isEnabled());
        QVERIFY(!actionNamed(bar.prMenu(), "claudeGitCommit")->isEnabled());

        // A file changed in a terminal: seen at the next look.
        QVERIFY(write(r + "/a.txt", "line 1\n"));
        bar.refresh();
        QTRY_VERIFY(!bar.statLabel()->isHidden());
        QVERIFY(bar.statLabel()->text().contains("−2"));
    }

    // In the dock: about the folder chosen for Claude, else the project's,
    // else the workspace; a prompt from the bar leaves the composer be.
    void inThePanel()
    {
        ClaudeCodePanel panel;
        panel.setDefaultDirectory(plain);
        ClaudeGitBar* bar = panel.gitBar();
        QVERIFY(bar != nullptr);
        QCOMPARE(panel.gitDirectory(), plain);
        QVERIFY(settled(bar));
        QVERIFY(bar->isHidden());

        panel.setProjectDirectory(repo);
        QCOMPARE(panel.gitDirectory(), repo);
        QCOMPARE(panel.workingDirectory(), plain);   // Claude still works in the workspace
        QVERIFY(settled(bar));
        QVERIFY(!bar->isHidden());
        QCOMPARE(bar->folderLabel()->text(), QStringLiteral("amp repo"));

        panel.composer()->setPlainText("half a thought");
        bar->prButton()->click();
        QCOMPARE(panel.composer()->toPlainText(), QStringLiteral("half a thought"));
        QVERIFY2(panel.transcriptText().contains("not found"), qPrintable(panel.transcriptText()));   // no claude here

        // The menu's setting.
        QMenu* menu = panel.findChild<QMenu*>();
        QAction* setting = nullptr;
        for (QMenu* m : panel.findChildren<QMenu*>())
            for (QAction* a : m->actions())
                if (a->objectName() == "claudeGitStatus") {
                    setting = a;
                    menu = m;
                }
        QVERIFY(setting != nullptr && setting->isCheckable());
        emit menu->aboutToShow();
        QVERIFY(setting->isChecked());
        setting->trigger();
        QVERIFY(!ClaudeGitBar::isOn());
        QVERIFY(bar->isHidden());
        setting->trigger();
        QVERIFY(ClaudeGitBar::isOn());
        QVERIFY(settled(bar));
        QVERIFY(!bar->isHidden());

        // A folder chosen for Claude comes first.
        panel.setWorkingDirectory(plain + "/..");
        QCOMPARE(panel.gitDirectory(), dir.path());
        QVERIFY(settled(bar));
        QVERIFY(bar->isHidden());
    }

    // The project opened in Qucs-S is what the bars are about.
    void theApplicationSaysWhichProject()
    {
        const QString project = dir.filePath("board_prj");
        QVERIFY(makeRepo(project));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.projsDir.setPath(dir.filePath("workspace"));
        QDir().mkpath(dir.filePath("workspace"));
        QucsApp app(false);
        MainGuard guard(&app);
        auto* tabs = app.findChild<ClaudeCodeTabs*>();
        QVERIFY(tabs != nullptr && tabs->current() != nullptr);
        QVERIFY(tabs->current()->projectDirectory().isEmpty());
        app.openProject(project);
        QCOMPARE(tabs->current()->projectDirectory(), QDir(project).absolutePath());
        QCOMPARE(tabs->current()->gitDirectory(), QDir(project).absolutePath());
        app.slotMenuProjClose();
        QVERIFY(tabs->current()->projectDirectory().isEmpty());
    }
};

QTEST_MAIN(TestClaudeGit)
#include "test_claude_git.moc"
