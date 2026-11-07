/*
 * How much git work each thing the windows do takes.
 *
 * As a test (ctest): the number of git programs each one starts, counted by a
 * git that keeps a list - the same on every machine, so no time limit to go
 * red on a slow runner (3972088, c6c7eb2) - and, where the work is ours and
 * not git's, how it grows: each size's best of five, one size after the other,
 * in Release alone.
 *
 * As a benchmark: QUCS_GIT_BENCH=<a repository> prints, for each, the programs
 * it starts and its best time of five (make_big_repo.py makes a large
 * repository; a repository named here is only looked at, nothing in it is
 * changed - but a window's own look). See
 * docs/feature_gaps/2026-11-07-git-integration-deep-dive.md.
 */

#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QProcess>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTreeView>
#include <QTreeWidget>

#include <functional>

#include "claudegitbar.h"
#include "gitgraph.h"
#include "githistory.h"
#include "gitrepo.h"
#include "gitstatus.h"
#include "gitui.h"
#include "isolated_settings.h"
#include "schematicdiff.h"

using namespace qucs_s::git;

namespace {

bool slowBuild()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
#ifndef NDEBUG
    return true;
#else
    return false;
#endif
}

bool write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString canonical(const QString& path)
{
    return QFileInfo(path).canonicalFilePath();
}

QString realGit()
{
    return QStandardPaths::findExecutable(QStringLiteral("git"));
}

bool gitIn(const QString& dir, const QStringList& args)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(realGit(), args);
    return p.waitForFinished(60000) && p.exitCode() == 0;
}

} // namespace

class TestGitPerf : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString counter, counted;

    // The git programs \a work starts (a first look at a repository's
    // guards let go by, as a second look finds them known).
    int programsOf(const std::function<void()>& work)
    {
        QFile::remove(counted);
        qputenv("QUCS_GIT", QFile::encodeName(counter));
        work();
        qunsetenv("QUCS_GIT");
        return int(readAll(counted).count('\n'));
    }

    QString listOf()
    {
        return QString::fromUtf8(readAll(counted));
    }

    // A repository of 20 files, some changed and new, with an upstream.
    QString makeRepo()
    {
        const QString origin = canonical(dir.path()) + "/origin";
        const QString repo = canonical(dir.path()) + "/repo";
        QDir().mkpath(origin);
        if (!gitIn(origin, {"init", "-q"})) return {};
        for (int i = 0; i < 20; ++i) write(origin + QStringLiteral("/f%1.txt").arg(i), "one\ntwo\n");
        write(origin + "/a.sch", "<Qucs Schematic 26.1.1>\n<Components>\n</Components>\n");
        gitIn(origin, {"add", "-A"});
        gitIn(origin, {"commit", "-q", "-m", "first"});
        for (int i = 0; i < 3; ++i) gitIn(origin, {"commit", "-q", "--allow-empty", "-m", QStringLiteral("next %1").arg(i)});
        if (!gitIn(canonical(dir.path()), {"clone", "-q", origin, repo})) return {};
        gitIn(repo, {"checkout", "-q", "-b", "feature"});
        write(repo + "/f1.txt", "changed\n");
        write(repo + "/f2.txt", "changed\n");
        gitIn(repo, {"add", "f2.txt"});
        write(repo + "/new.txt", "new\n");
        return repo;
    }

    void bench(const QString& repo)
    {
        QTextStream out(stdout);
        const auto row = [&](const QString& what, const std::function<void()>& work, int times = 5) {
            const int programs = programsOf(work);
            qint64 best = -1;
            for (int i = 0; i < times; ++i) {
                QElapsedTimer t;
                t.start();
                work();
                const qint64 took = t.nsecsElapsed();
                if (best < 0 || took < best) best = took;
            }
            out << QStringLiteral("| %1 | %2 | %3 |\n").arg(what).arg(programs).arg(best / 1e6, 0, 'f', 1);
            out.flush();
        };
        out << "| What | git programs | best of 5, ms |\n|---|---:|---:|\n";
        const QString someFile = [&] {
            const QList<ChangedFile> files = changedFiles(repo, "HEAD");
            return files.isEmpty() ? QStringLiteral("README.md") : files.first().path;
        }();
        const QString untracked = [&] {
            for (const Entry& e : read(repo).entries)
                if (e.untracked && !e.isFolder()) return e.path;
            return QString();
        }();
        const QString modified = [&] {
            for (const Entry& e : read(repo).entries)
                if (!e.untracked && !e.ignored && !e.isFolder() && e.unstaged != QLatin1Char('.')) return e.path;
            return QString();
        }();
        (void)read(repo);   // (guards known)
        row("read()", [&] { (void)read(repo); });
        row("Tracker::readNow", [&] { (void)Tracker::instance()->readNow(repo); });
        row("status(): the dock's bar, every 10 s", [&] { (void)status(repo); });
        row("Tracker: 1 file's entry, as a view draws", [&] { (void)Tracker::instance()->entryOf(repo + "/" + someFile); });
        Status s = status(repo);
        row("diff(status, a file)", [&] { (void)qucs_s::git::diff(s, someFile); });
        row("diff(status): All changes", [&] { (void)qucs_s::git::diff(s); });
        row("diff(root, a file): a click in the Commit window", [&] { (void)qucs_s::git::diff(repo, modified.isEmpty() ? someFile : modified, DiffOf::Unstaged); });
        row("diff(root, staged)", [&] { (void)qucs_s::git::diff(repo, {}, DiffOf::Staged); });
        row("diff(root): all", [&] { (void)qucs_s::git::diff(repo); });
        if (!untracked.isEmpty()) row("diff(root, an untracked file)", [&] { (void)qucs_s::git::diff(repo, untracked); });
        row("schematicChangesOf(root)", [&] { (void)schematicChangesOf(repo); });
        row("log(root), 200", [&] { (void)log(repo); });
        row("log(a file), 200, followed", [&] { (void)log(repo, someFile); });
        HistoryQuery all;
        row("history(All), a page of 500", [&] { (void)qucs_s::git::history(repo, all); });
        HistoryQuery second;
        second.skip = 500;
        row("history(All), the next page", [&] { (void)qucs_s::git::history(repo, second); });
        row("branches()", [&] { (void)branches(repo); });
        row("tags()", [&] { (void)tags(repo); });
        row("stashes()", [&] { (void)stashes(repo); });
        row("changedFiles(HEAD)", [&] { (void)changedFiles(repo, "HEAD"); });
        row("commitDiff(HEAD)", [&] { (void)commitDiff(repo, "HEAD"); });
        row("schematicChangesIn(HEAD)", [&] { (void)schematicChangesIn(repo, "HEAD"); });
        row("commitMessage(HEAD)", [&] { (void)commitMessage(repo, "HEAD"); });
        row("blame(a file)", [&] { (void)blame(repo, someFile); });
        row("fileAt(HEAD, a file)", [&] { (void)fileAt(repo, "HEAD", someFile); });
        // On the GUI thread: what a window waits for.
        row("History window: opened", [&] { HistoryView v(repo); });
        {
            HistoryView view(repo);
            view.resize(1200, 800);
            view.show();
            QCoreApplication::processEvents();
            row("History: reload()", [&] { view.reload(); });
            row("History: refresh(), on any change of the repository", [&] { view.refresh(); });
            int at = 0;
            row("History: a commit chosen", [&] {
                at = (at + 7) % 200;
                view.selectRow(at + 1);
            });
            row("History: uncommitted row chosen", [&] {
                view.selectRow(1);
                view.selectRow(0);
            });
            row("History: its menu", [&] { delete view.menuFor(2); });
            row("History: 100 rows drawn", [&] { (void)view.grab(); });
            row("History: Show More", [&] { view.loadMore(); });
        }
        row("Commit window: opened", [&] { CommitDialog d(repo); });
        row("Blame window: opened", [&] { BlameDialog d(repo, someFile); }, 3);
        row("Changes window: opened (All changes)", [&] { GitChangesDialog d(s); }, 3);
        row("A file's Git menu, read within 3 s", [&] {
            QMenu m;
            Commands::instance()->fillFileMenu(&m, repo + "/" + someFile);
        });
        row("The branch menu", [&] {
            QMenu m;
            Commands::instance()->fillBranchMenu(&m, repo);
        });
        // Ours alone: no git.
        HistoryQuery many;
        many.max = 50000;
        const Result text = run(repo, {"log", "--decorate=full", "--format=%H%x00%h%x00%an%x00%ae%x00%aI%x00%s%x00%D%x00%P%x00%cn%x00%ce%x00%cI%x00",
                                       "--date-order", "-n", "50000", "--branches", "--remotes", "--tags", "HEAD"}, true, 120000);
        QList<Commit> commits;
        row("readCommits: what git wrote for up to 50,000", [&] { commits = readCommits(text.out); });
        out << "commits read: " << commits.size() << "\n";
        row("GraphLayout: as many rows", [&] {
            GraphLayout layout;
            for (const Commit& c : commits) (void)layout.next(c.hash, c.parents);
        });
        row("readStatus: a status read", [&] {
            const Result st = run(repo, {"status", "--porcelain=v2", "-z", "--branch", "--untracked-files=normal", "--ignored=matching"}, true);
            Repository r;
            readStatus(st.out, r);
        });
    }

private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        QSKIP("its counting git is a shell script");
#endif
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        if (program().isEmpty()) QSKIP("git is not installed");
        qputenv("QUCS_TRASH_DIR", QFile::encodeName(dir.filePath("trash")));
        QVERIFY(write(dir.filePath("gitconfig"), "[user]\n\tname = Tester\n\temail = tester@example.com\n[init]\n\tdefaultBranch = main\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(dir.filePath("gitconfig")));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        counted = canonical(dir.path()) + "/programs";
        counter = canonical(dir.path()) + "/git.sh";
        QVERIFY(write(counter, "#!/bin/sh\necho \"$*\" >> '" + QFile::encodeName(counted) + "'\nexec '" + QFile::encodeName(realGit()) + "' \"$@\"\n"));
        QVERIFY(QFile::setPermissions(counter, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    // The programs each takes: 1 where a read took 2, 5 where the dock's bar
    // took 14, none a new file each where a tree of them took one a file. Any
    // machine counts the same.
    void theProgramsEachStarts()
    {
        const QString repo = makeRepo();
        QVERIFY(!repo.isEmpty());
        for (int i = 0; i < 30; ++i) QVERIFY(write(repo + QStringLiteral("/many/new%1.txt").arg(i), "new\n"));
        const auto schematic = [](const char* value) {
            return QByteArray("<Qucs Schematic 26.1.1>\n<Components>\n  <R R1 1 100 100 15 -26 0 1 \"") + value + "\" 1>\n</Components>\n";
        };
        for (int i = 0; i < 5; ++i) QVERIFY(write(repo + QStringLiteral("/s%1.sch").arg(i), schematic("1")));
        QVERIFY(gitIn(repo, {"add", QStringLiteral("s0.sch"), "s1.sch", "s2.sch", "s3.sch", "s4.sch"}));
        QVERIFY(gitIn(repo, {"commit", "-q", "-m", "the schematics"}));
        for (int i = 0; i < 5; ++i) QVERIFY(write(repo + QStringLiteral("/s%1.sch").arg(i), schematic("2")));
        (void)read(repo);   // (its guards known, as they are but once in 30 s)
#define AT_MOST(n, ...) { const int programs = programsOf([&] { __VA_ARGS__; }); QVERIFY2(programs <= (n), qPrintable(QStringLiteral(#__VA_ARGS__ ": %1 programs\n").arg(programs) + listOf())); }
        AT_MOST(1, (void)read(repo));
        AT_MOST(5, QCOMPARE(status(repo).upstream, QString()));
        AT_MOST(2, QVERIFY(qucs_s::git::diff(repo, "f1.txt", DiffOf::Unstaged).contains("+changed")));
        AT_MOST(1, QVERIFY(qucs_s::git::diff(repo, "new.txt").contains("+new")));
        AT_MOST(3, QVERIFY(qucs_s::git::diff(repo).contains("many/new29.txt")));
        AT_MOST(2, QVERIFY(schematicChangesOf(repo).contains("s4.sch")));
        const Repository looked = read(repo);
        AT_MOST(1, QVERIFY(schematicChangesOf(repo, {}, DiffOf::Head, &looked).contains("s4.sch")));
        AT_MOST(4, QVERIFY(schematicChangesIn(repo, "HEAD").contains("s4.sch")));
        const QString hash = log(repo).first().hash;
        AT_MOST(4, QCOMPARE(changedFiles(repo, hash).size(), qsizetype(6)));
        AT_MOST(0, QCOMPARE(changedFiles(repo, hash).size(), qsizetype(6)));

        // The History, on the GUI thread.
        Tracker::instance()->readNow(repo);
        HistoryView view(repo);
        AT_MOST(0, delete view.menuFor(1));
        Tracker::instance()->readNow(repo);
        AT_MOST(2, view.refresh());
        Tracker::instance()->readNow(repo);
        AT_MOST(12, view.reload());
        Tracker::instance()->readNow(repo);
        // (Uncommitted changes in row 0, its commits below.)
        AT_MOST(5, view.selectRow(1));
        view.selectRow(2);
        Tracker::instance()->readNow(repo);
        AT_MOST(2 + 1 + 3, view.selectRow(0));
#undef AT_MOST
    }

    void benchmark()
    {
        if (!qEnvironmentVariableIsSet("QUCS_GIT_BENCH")) QSKIP("QUCS_GIT_BENCH names a repository");
        bench(qEnvironmentVariable("QUCS_GIT_BENCH"));
    }
};

QTEST_MAIN(TestGitPerf)
#include "test_git_perf.moc"
