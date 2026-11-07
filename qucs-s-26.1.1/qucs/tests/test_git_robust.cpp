/*
 * The git integration where it is hard: file names git would read as
 * patterns, commands stopped midway, links, a repository's own programs,
 * commits written by hand, every state a repository can be in. No window:
 * gitrepo.h and gitstatus.h as the windows and Claude's tools call them. See
 * docs/feature_gaps/2026-11-07-git-integration-deep-dive.md.
 *
 * Every repository is in a temporary folder.
 */

#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "gitrepo.h"
#include "gitstatus.h"
#include "gitui.h"
#include "isolated_settings.h"
#include "schematicdiff.h"

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

using namespace qucs_s::git;

namespace {

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

// git in \a dir, by the tests' own hand: its output, or a null array when it
// failed.
QByteArray gitIn(const QString& dir, const QStringList& args, const QByteArray& input = {})
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(10000)) return {};
    if (!input.isEmpty()) p.write(input);
    p.closeWriteChannel();
    if (!p.waitForFinished(60000) || p.exitCode() != 0) {
        qWarning() << "git" << args << "failed in" << dir << ":" << p.readAllStandardError() << p.readAllStandardOutput();
        return {};
    }
    QByteArray out = p.readAllStandardOutput();
    if (out.isNull()) out = "";
    return out;
}

// The entry of \a path, or null.
const Entry* entry(const Repository& r, const QString& path)
{
    for (const Entry& e : r.entries)
        if (e.path == path) return &e;
    return nullptr;
}

// A program: writes its name into \a marks each time it runs, and fails.
QString evilProgram(const QString& folder, const QString& marks, const QString& name)
{
    const QString path = folder + "/" + name + ".sh";
    write(path, "#!/bin/sh\necho ran >> '" + QFile::encodeName(marks + "/" + name) + "'\nexit 1\n");
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
    return path;
}

// \a work on a thread of its own, up to \a ms: whether it came back. (One
// that hangs is left behind: the test has failed by then.)
bool cameBack(const std::function<void()>& work, int ms = 15000)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([work, done] {
        work();
        done->store(true);
    }).detach();
    QElapsedTimer t;
    t.start();
    while (!done->load() && t.elapsed() < ms) QTest::qWait(5);
    return done->load();
}

} // namespace

class TestGitRobust : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    int made = 0;

    QString folder(const QString& name)
    {
        const QString path = canonical(dir.path()) + "/" + name + QString::number(++made);
        QDir().mkpath(path);
        return canonical(path);
    }

    // A repository: a.txt in one commit.
    QString makeRepo(const QString& name = QStringLiteral("repo"))
    {
        const QString repo = folder(name);
        if (gitIn(repo, {"init", "-q"}).isNull()) return {};
        write(repo + "/a.txt", "one\ntwo\nthree\n");
        gitIn(repo, {"add", "-A"});
        gitIn(repo, {"commit", "-q", "-m", "first"});
        return repo;
    }

    // Two sides that changed \a files each its way: the merge in conflict.
    bool makeConflict(const QString& repo, const QHash<QString, QPair<QByteArray, QByteArray>>& files)
    {
        for (auto it = files.cbegin(); it != files.cend(); ++it) write(repo + "/" + it.key(), "base\n");
        gitIn(repo, {"add", "-A"});
        gitIn(repo, {"commit", "-q", "-m", "base"});
        gitIn(repo, {"checkout", "-q", "-b", "theirs"});
        for (auto it = files.cbegin(); it != files.cend(); ++it) write(repo + "/" + it.key(), it.value().second);
        gitIn(repo, {"commit", "-q", "-a", "-m", "theirs"});
        gitIn(repo, {"checkout", "-q", "main"});
        for (auto it = files.cbegin(); it != files.cend(); ++it) write(repo + "/" + it.key(), it.value().first);
        gitIn(repo, {"commit", "-q", "-a", "-m", "ours"});
        return !run(repo, {"merge", "--no-edit", "theirs"}).ok() && read(repo).conflictCount() == files.size();
    }

private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        QSKIP("the programs here are shell scripts, the names not all Windows' own");
#endif
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        if (program().isEmpty()) QSKIP("git is not installed");
        qputenv("QUCS_TRASH_DIR", QFile::encodeName(dir.filePath("trash")));
        qputenv("GIT_CEILING_DIRECTORIES", QFile::encodeName(canonical(dir.path())) + QByteArray(1, QDir::listSeparator().toLatin1())
                                               + QFile::encodeName(dir.path()));
        QVERIFY(write(dir.filePath("gitconfig"),
                      "[user]\n\tname = Tester\n\temail = tester@example.com\n[init]\n\tdefaultBranch = main\n"
                      "[commit]\n\tgpgsign = false\n[tag]\n\tgpgsign = false\n[protocol \"file\"]\n\tallow = always\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(dir.filePath("gitconfig")));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }

    // ------------------------------------------------------------------
    // Names git would read as patterns: a file's own, and no other.

    void aFileNameIsNoPattern()
    {
        const QString repo = makeRepo();
        const QStringList names{"amp[1].sch", "amp1.sch", ":!x", "other.sch", "star*", "stars", "a b.sch", "-dash.sch", ":(top)t"};
        for (const QString& n : names) QVERIFY(write(repo + "/" + n, "one\n"));
        QVERIFY(!gitIn(repo, {"add", "-A"}).isNull());
        QVERIFY(!gitIn(repo, {"commit", "-q", "-m", "names"}).isNull());
        const auto editAll = [&] {
            for (const QString& n : names) write(repo + "/" + n, "edit\n");
        };
        const auto text = [&](const QString& n) { return readAll(repo + "/" + n); };

        // Discarded: that file's changes, none other's.
        editAll();
        QVERIFY(discard(repo, {"amp[1].sch"}).ok());
        QCOMPARE(text("amp[1].sch"), QByteArray("one\n"));
        QCOMPARE(text("amp1.sch"), QByteArray("edit\n"));
        // (":!x" to git: everything but x.)
        QVERIFY(discard(repo, {":!x"}).ok());
        QCOMPARE(text(":!x"), QByteArray("one\n"));
        QCOMPARE(text("other.sch"), QByteArray("edit\n"));
        QVERIFY(discard(repo, {"star*"}).ok());
        QCOMPARE(text("star*"), QByteArray("one\n"));
        QCOMPARE(text("stars"), QByteArray("edit\n"));

        // Staged and unstaged: that file alone.
        editAll();
        QVERIFY(stage(repo, {"star*"}).ok());
        Repository r = read(repo);
        QVERIFY(entry(r, "star*") != nullptr && entry(r, "star*")->hasStaged());
        QVERIFY(entry(r, "stars") != nullptr && !entry(r, "stars")->hasStaged());
        QVERIFY(stage(repo, {"stars", ":!x"}).ok());
        r = read(repo);
        QVERIFY(entry(r, "other.sch") != nullptr && !entry(r, "other.sch")->hasStaged());
        QVERIFY(entry(r, ":!x")->hasStaged());
        QVERIFY(unstage(repo, {"star*"}).ok());
        r = read(repo);
        QVERIFY(!entry(r, "star*")->hasStaged());
        QVERIFY(entry(r, "stars")->hasStaged());
        QVERIFY(stage(repo, {":(top)t"}).ok());
        QVERIFY(entry(read(repo), ":(top)t")->hasStaged());
        QVERIFY(unstage(repo, {}).ok());

        // Its diff, its history.
        const QString one = diff(repo, "amp[1].sch", DiffOf::Unstaged);
        QVERIFY(one.contains("amp[1].sch"));
        QVERIFY(!one.contains("amp1.sch"));
        const QString dash = diff(repo, "-dash.sch", DiffOf::Unstaged);
        QVERIFY(dash.contains("+edit"));
        QVERIFY(discard(repo, {}).ok());
        QVERIFY(write(repo + "/amp1.sch", "only this\n"));
        QVERIFY(commit(repo, "amp1 alone", false, {"amp1.sch"}).ok());
        const QList<Commit> history = log(repo, "amp[1].sch");
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().subject, QStringLiteral("names"));
        HistoryQuery q;
        q.path = "amp[1].sch";
        QCOMPARE(qucs_s::git::history(repo, q).size(), qsizetype(1));
        QCOMPARE(blame(repo, ":!x").size(), qsizetype(1));

        // A new file whose name begins with ':': found, counted, to the trash.
        QVERIFY(write(repo + "/:new.txt", "new\nfile\n"));
        QVERIFY(diff(repo, ":new.txt").contains("+file"));
        const Status counted = status(repo);
        for (const FileChange& f : counted.changes)
            if (f.path == QLatin1String(":new.txt")) QCOMPARE(f.added, 2);
        QVERIFY(qucs_s::git::diff(counted, ":new.txt").contains("+file"));
        QStringList trashed;
        QVERIFY(discard(repo, {":new.txt"}, &trashed).ok());
        QCOMPARE(trashed.size(), qsizetype(1));
        QVERIFY(!QFileInfo::exists(repo + "/:new.txt"));
        QVERIFY(write(repo + "/:ignored", "x\n"));
        QVERIFY(ignore(repo, ":ignored"));
        QVERIFY(entry(read(repo), ":ignored") != nullptr && entry(read(repo), ":ignored")->ignored);
        QVERIFY(QFile::remove(repo + "/.gitignore"));
        QVERIFY(QFile::remove(repo + "/:ignored"));

        // Stops being followed: that file.
        QVERIFY(untrack(repo, {"star*"}).ok());
        QVERIFY(!gitIn(repo, {"ls-files", "--error-unmatch", "--", "stars"}).isNull());
        QVERIFY(QFileInfo::exists(repo + "/star*"));
        // A folder and the whole tree still taken.
        QVERIFY(write(repo + "/fold[er]/in.sch", "new\n"));
        QVERIFY(write(repo + "/folde/in.sch", "new\n"));
        QVERIFY(stage(repo, {"fold[er]"}).ok());
        r = read(repo);
        QVERIFY(entry(r, "fold[er]/in.sch") != nullptr && entry(r, "fold[er]/in.sch")->hasStaged());
        QVERIFY(entry(r, "folde/") != nullptr);
        QVERIFY(stage(repo, {"."}).ok());
        QVERIFY(!gitIn(repo, {"ls-files", "--error-unmatch", "--", "star*"}).isNull());
        QVERIFY(commit(repo, "more", false, {"fold[er]"}).ok());
        // Read by the commit, a file's alone.
        QVERIFY(commitDiff(repo, "HEAD", "fold[er]/in.sch").contains("+new"));
        QVERIFY(show(repo, "HEAD", "fold[er]/in.sch").contains("+new"));
        // A hook's own patterns still work: only Qucs-S's paths are literal.
        QVERIFY(write(repo + "/.git/hooks/pre-commit", "#!/bin/sh\ngit diff --cached --name-only -- '*.hook' > hook-saw\ngit check-ignore -q a.txt\ntest $? -ne 128\n"));
        QVERIFY(QFile::setPermissions(repo + "/.git/hooks/pre-commit", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(repo + "/seen.hook", "x\n"));
        const Result hooked = commit(repo, "hooked", false, {"seen.hook"});
        QVERIFY2(hooked.ok(), qPrintable(hooked.error()));
        QCOMPARE(readAll(repo + "/hook-saw"), QByteArray("seen.hook\n"));
    }

    // Out of the index, on the disk: two entries, one file - back as committed.
    void aFileNoLongerFollowedIsDiscardedBack()
    {
        const QString repo = makeRepo();
        QVERIFY(untrack(repo, {"a.txt"}).ok());
        QVERIFY(write(repo + "/a.txt", "edited since\n"));
        const Result r = discard(repo, {"a.txt"});
        QVERIFY2(r.ok(), qPrintable(r.error()));
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\ntwo\nthree\n"));
        QCOMPARE(read(repo).changedCount(), 0);
        // By everything, too.
        QVERIFY(untrack(repo, {"a.txt"}).ok());
        QVERIFY(discard(repo, {}).ok());
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\ntwo\nthree\n"));
    }

    // A schematic's lines, damaged: nothing to cut from.
    void aSchematicLineOfOneCharacter()
    {
        for (const char* text : {"<\n", ">\n", "<Properties>\n>\n</Properties>\n", "<Properties>\n<\n</Properties>\n", "<>\n<Components>\n>\n"}) {
            (void)schematicChanges(QString::fromLatin1(text), QStringLiteral("<Qucs Schematic 1>\n"));
            (void)schematicChanges(QStringLiteral("<Qucs Schematic 1>\n"), QString::fromLatin1(text));
        }
        // A setting still read.
        const QStringList lines = schematicChanges(QStringLiteral("<Properties>\n<Grid=10>\n</Properties>\n"), QStringLiteral("<Properties>\n<Grid=20>\n</Properties>\n"));
        QVERIFY2(lines.join(QLatin1Char('\n')).contains("Grid"), qPrintable(lines.join(QLatin1Char('\n'))));
    }

    void aConflictsNameIsNoPattern()
    {
        const QString repo = makeRepo();
        QVERIFY(makeConflict(repo, {{"x[1].txt", {"ours one\n", "theirs one\n"}}, {"x1.txt", {"ours two\n", "theirs two\n"}}}));
        const ConflictVersions v = conflictVersions(repo, "x[1].txt");
        QVERIFY(v.inConflict);
        QCOMPARE(*v.ours, QStringLiteral("ours one\n"));
        QCOMPARE(*v.theirs, QStringLiteral("theirs one\n"));
        QVERIFY(resolve(repo, "x[1].txt", "theirs").ok());
        QCOMPARE(readAll(repo + "/x[1].txt"), QByteArray("theirs one\n"));
        const Repository r = read(repo);
        QVERIFY(entry(r, "x1.txt") != nullptr && entry(r, "x1.txt")->conflicted);
    }

    // ------------------------------------------------------------------
    // Stopped midway: what git holds is let go.

    void aStoppedCommitLeavesNoLock()
    {
        const QString repo = makeRepo();
        const QString started = repo + "/.git/hook-started";
        QVERIFY(write(repo + "/.git/hooks/pre-commit", "#!/bin/sh\ntouch '" + QFile::encodeName(started) + "'\nsleep 4\n"));
        QVERIFY(QFile::setPermissions(repo + "/.git/hooks/pre-commit", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(repo + "/a.txt", "changed\n"));

        // Run by a Job (a commit in the windows, in Claude's tool) and stopped...
        {
            Job job(repo, {"commit", "-F", "-", "--", "a.txt"}, nullptr, "message");
            QSignalSpy finished(&job, &Job::finished);
            job.start();
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(started), 10000);
            QVERIFY(QFileInfo::exists(repo + "/.git/index.lock"));
            job.cancel();
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
            QCOMPARE(job.result().exitCode, -1);
        }
        QVERIFY2(!QFileInfo::exists(repo + "/.git/index.lock"), "a stopped commit left index.lock: every git command after it fails");

        // ...or past its time.
        QFile::remove(started);
        const Result late = run(repo, {"commit", "-F", "-", "--", "a.txt"}, false, 1000, "message");
        QCOMPARE(late.exitCode, -1);
        QVERIFY(!QFileInfo::exists(repo + "/.git/index.lock"));
        QVERIFY(stage(repo, {"a.txt"}).ok());

        // Gone with a job that runs: a window closed, the program ended.
        QFile::remove(started);
        QVERIFY(write(repo + "/a.txt", "again\n"));
        {
            Job job(repo, {"commit", "-F", "-", "--", "a.txt"}, nullptr, "message");
            job.start();
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(started), 10000);
        }
        QVERIFY(!QFileInfo::exists(repo + "/.git/index.lock"));
    }

    void aStoppedCloneLeavesNoFolder()
    {
        const QString parent = folder("clones");
        const QString started = parent + "/ssh-started";
        const QString ssh = parent + "/ssh.sh";
        QVERIFY(write(ssh, "#!/bin/sh\ntouch '" + QFile::encodeName(started) + "'\nsleep 4\n"));
        QVERIFY(QFile::setPermissions(ssh, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("GIT_SSH_COMMAND", QFile::encodeName(ssh));
        const QString target = parent + "/cloned";
        {
            Job job(parent, cloneArgs("ssh://example.invalid/some.git", target));
            QSignalSpy finished(&job, &Job::finished);
            job.start();
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(started), 10000);
            QVERIFY(QFileInfo::exists(target));
            job.cancel();
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        }
        qunsetenv("GIT_SSH_COMMAND");
        // (Else a second try says "already exists and is not an empty directory".)
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(target), 3000);
    }

    // A job with no folder, no git, or gone before it ends.
    void aJobThatCannotRun()
    {
        Job gone(dir.filePath("no-such-folder"), {"status"});
        QSignalSpy goneDone(&gone, &Job::finished);
        gone.start();
        QTRY_COMPARE_WITH_TIMEOUT(goneDone.size(), 1, 5000);
        QVERIFY(!gone.result().ok());

        const QString repo = makeRepo();
        qputenv("QUCS_GIT", "/no/such/git");
        QVERIFY(program().isEmpty());
        Job none(repo, {"status"});
        QSignalSpy noneDone(&none, &Job::finished);
        none.start();
        QTRY_COMPARE_WITH_TIMEOUT(noneDone.size(), 1, 5000);
        QCOMPARE(none.result().exitCode, -1);
        QVERIFY(!read(repo).valid);
        QVERIFY(!status(repo).repository);
        QCOMPARE(run(repo, {"status"}).exitCode, -1);
        QVERIFY(!run(repo, {"status"}).error().isEmpty());
        QVERIFY(log(repo).isEmpty());
        QVERIFY(Tracker::instance()->readNow(repo) == nullptr);
        qunsetenv("QUCS_GIT");
        QVERIFY(!program().isEmpty());
    }

    // ------------------------------------------------------------------
    // A link is a link: what it points at is not read.

    void aLinkIsNotFollowed()
    {
        const QString repo = makeRepo();
        const QString secret = folder("elsewhere") + "/secret.txt";
        QVERIFY(write(secret, "the secret\nline\nline\nline\nline\n"));
        QVERIFY(QFile::link(secret, repo + "/link"));
        const Status s = status(repo);
        const auto of = [&](const QString& path) -> const FileChange* {
            for (const FileChange& f : s.changes)
                if (f.path == path) return &f;
            return nullptr;
        };
        QVERIFY(of("link") != nullptr);
        QCOMPARE(of("link")->added, 1);   // (as git counts one: the link's own text)
        const QString shown = qucs_s::git::diff(s, "link");
        QVERIFY2(!shown.contains("the secret"), qPrintable(shown));
        QVERIFY(shown.contains(secret));
        QVERIFY(!qucs_s::git::diff(s).contains("the secret"));
    }

    void aLinkToAPipeOrADeviceIsNotRead()
    {
#ifndef Q_OS_WIN
        const QString repo = makeRepo();
        const QString pipe = folder("pipes") + "/pipe";
        QCOMPARE(mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
        QVERIFY(QFile::link(pipe, repo + "/to-pipe"));
        auto shown = std::make_shared<Status>();
        QVERIFY2(cameBack([repo, shown] { *shown = status(repo); }), "status hangs on a link to a pipe");
        QVERIFY(cameBack([shown] { qucs_s::git::diff(*shown); }));
        QVERIFY(cameBack([pipe] { hasConflictMarkers(pipe); }));
        QVERIFY(cameBack([repo] { hasConflictMarkers(repo); }));
        // A device without end: read, it would fill the memory.
        const QString device = folder("devices");
        QVERIFY(init(device).ok());
        QVERIFY(QFile::link("/dev/zero", device + "/to-zero"));
        auto zero = std::make_shared<Status>();
        QVERIFY(cameBack([device, zero] { *zero = status(device); }));
        QVERIFY(cameBack([zero] { qucs_s::git::diff(*zero); }));
        QVERIFY(cameBack([device] { hasConflictMarkers(device + "/to-zero"); }));
#endif
    }

    // ------------------------------------------------------------------
    // The repository's own programs, when Qucs-S only looks.

    void signaturesAreNotChecked()
    {
        const QString repo = makeRepo();
        const QString marks = folder("marks");
        const QString tree = QString::fromLatin1(gitIn(repo, {"rev-parse", "HEAD^{tree}"})).trimmed();
        const QString parent = QString::fromLatin1(gitIn(repo, {"rev-parse", "HEAD"})).trimmed();
        const QByteArray signedCommit = "tree " + tree.toLatin1() + "\nparent " + parent.toLatin1()
                                        + "\nauthor A <a@a> 1700000000 +0000\ncommitter A <a@a> 1700000000 +0000\n"
                                          "gpgsig -----BEGIN PGP SIGNATURE-----\n \n iQ==\n -----END PGP SIGNATURE-----\n\nsigned\n";
        const QString hash = QString::fromLatin1(gitIn(repo, {"hash-object", "-t", "commit", "-w", "--stdin"}, signedCommit)).trimmed();
        QVERIFY(!hash.isEmpty());
        QVERIFY(!gitIn(repo, {"update-ref", "HEAD", hash}).isNull());
        QVERIFY(write(repo + "/a.txt", "stash me\n"));
        QVERIFY(!gitIn(repo, {"stash", "push", "-q", "-m", "kept"}).isNull());
        QVERIFY(!gitIn(repo, {"config", "gpg.program", evilProgram(marks, marks, "gpg")}).isNull());
        QVERIFY(!gitIn(repo, {"config", "log.showSignature", "true"}).isNull());

        const QList<Commit> commits = log(repo);
        QVERIFY(!commits.isEmpty());
        QCOMPARE(commits.first().hash, hash);
        QCOMPARE(commits.first().subject, QStringLiteral("signed"));
        QCOMPARE(commits.first().parents, QStringList{parent});
        HistoryQuery q;
        q.scope = HistoryQuery::Current;
        QCOMPARE(qucs_s::git::history(repo, q).first().hash, hash);
        QCOMPARE(stashes(repo).size(), qsizetype(1));
        QVERIFY(show(repo, "HEAD").contains("signed"));
        QVERIFY2(!QFileInfo::exists(marks + "/gpg"), "log.showSignature ran the repository's gpg.program");

        // Unsigned with the same setting and no such program: no lines of
        // its own in the answer.
        QVERIFY(!gitIn(repo, {"config", "--unset", "gpg.program"}).isNull());
        QCOMPARE(log(repo).first().hash, hash);
    }

    void aFilterNamedWithAnEqualsSign()
    {
        const QString repo = makeRepo();
        const QString marks = folder("marks");
        QVERIFY(write(repo + "/.gitattributes", "*.txt filter=a=b\n"));
        QVERIFY(!gitIn(repo, {"add", "-A"}).isNull());
        QVERIFY(!gitIn(repo, {"commit", "-q", "-m", "attributes"}).isNull());
        // (Its key has the '=' in it: -c would read "filter.a" as the key.)
        QVERIFY(write(repo + "/.git/config", readAll(repo + "/.git/config") + "[filter \"a=b\"]\n\tclean = "
                                                 + QFile::encodeName(evilProgram(marks, marks, "clean")) + "\n"));
        QVERIFY(write(repo + "/a.txt", "changed\n"));
        const Repository r = read(repo);
        QVERIFY(r.valid);
        QVERIFY(entry(r, "a.txt") != nullptr);
        QVERIFY(status(repo).repository);
        QVERIFY(diff(repo).contains("changed"));
        QVERIFY2(!QFileInfo::exists(marks + "/clean"), "a filter whose name holds '=' ran");
    }

    void aSubmodulesOwnProgramsAreNotRun()
    {
        const QString sub = makeRepo("sub");
        QVERIFY(write(sub + "/f.txt", "x\n"));
        QVERIFY(write(sub + "/.gitattributes", "f.txt filter=evil\n"));
        QVERIFY(!gitIn(sub, {"add", "-A"}).isNull());
        QVERIFY(!gitIn(sub, {"commit", "-q", "-m", "sub"}).isNull());
        const QString repo = makeRepo();
        QVERIFY(!gitIn(repo, {"submodule", "add", "-q", sub, "s"}).isNull());
        QVERIFY(!gitIn(repo, {"commit", "-q", "-m", "with its submodule"}).isNull());
        const QString marks = folder("marks");
        QVERIFY(!gitIn(repo + "/s", {"config", "filter.evil.clean", evilProgram(marks, marks, "clean")}).isNull());
        // (The same size: only a filter's answer tells it from the old one.)
        QVERIFY(write(repo + "/s/f.txt", "y\n"));
        const Repository r = read(repo);
        QVERIFY(r.valid);
        QVERIFY(status(repo).repository);
        QVERIFY2(!QFileInfo::exists(marks + "/clean"), "looking at a repository ran a filter its submodule's configuration names");
        // A submodule's own state, looked at from inside it: its .git a file.
        const Repository inside = read(repo + "/s");
        QVERIFY(inside.valid);
        QCOMPARE(inside.root, repo + "/s");
        QVERIFY(inside.gitDir.contains("modules"));
    }

    void aPartialClonesFetchIsNotRun()
    {
        // (GIT_NO_LAZY_FETCH is git 2.45's: an older git fetches, and this
        // setting cannot stop it - the write-up says so.)
        const QStringList version = QString::fromLatin1(gitIn(dir.path(), {"--version"})).section(QLatin1Char(' '), 2, 2).split(QLatin1Char('.'));
        if (version.value(0).toInt() < 2 || (version.value(0).toInt() == 2 && version.value(1).toInt() < 45)) QSKIP("git before 2.45");
        const QString origin = makeRepo("origin");
        QVERIFY(write(origin + "/a.txt", "the next one\n"));
        QVERIFY(!gitIn(origin, {"commit", "-q", "-a", "-m", "second"}).isNull());
        QVERIFY(!gitIn(origin, {"config", "uploadpack.allowFilter", "true"}).isNull());
        const QString repo = folder("partial") + "/clone";
        QVERIFY(!gitIn(dir.path(), {"clone", "-q", "--filter=blob:none", "file://" + origin, repo}).isNull());
        const QString marks = folder("marks");
        QVERIFY(!gitIn(repo, {"config", "remote.origin.url", "ssh://example.invalid/x"}).isNull());
        QVERIFY(!gitIn(repo, {"config", "core.sshCommand", evilProgram(marks, marks, "ssh")}).isNull());
        // (The blob of the first commit is not here: a diff would fetch it.)
        const QString older = commitDiff(repo, "HEAD");
        QVERIFY(status(repo).repository);
        QVERIFY(!show(repo, "HEAD").isNull());
        QVERIFY2(!QFileInfo::exists(marks + "/ssh"), qPrintable("looking at a partial clone ran its core.sshCommand: " + older));
    }

    // ------------------------------------------------------------------
    // A commit written by hand: its fields are its fields.

    void aCommitsFieldsHoldAnything()
    {
        const QString repo = makeRepo();
        const QString tree = QString::fromLatin1(gitIn(repo, {"rev-parse", "HEAD^{tree}"})).trimmed();
        const QString parent = QString::fromLatin1(gitIn(repo, {"rev-parse", "HEAD"})).trimmed();
        // Separators, and a row of its own made up in the subject.
        const QByteArray spoof = QByteArray(40, '0') + "\x1f" "0\x1f" "x\x1f" "x\x1f" "2020-01-01T00:00:00Z\x1f" "spoof\x1f\x1f"
                                 + QByteArray(40, 'f') + "\x1f" "x\x1f" "x\x1f" "2020-01-01T00:00:00Z";
        const QByteArray subject = "s1\x1fs2\x1e" + spoof;
        const QByteArray author = "A\x1f" "B\x1e" "C";
        const QByteArray object = "tree " + tree.toLatin1() + "\nparent " + parent.toLatin1() + "\nauthor " + author
                                  + " <a@a> 1700000000 +0000\ncommitter " + author + " <a@a> 1700000000 +0000\n\n" + subject + "\n";
        const QString hash = QString::fromLatin1(gitIn(repo, {"hash-object", "-t", "commit", "-w", "--literally", "--stdin"}, object)).trimmed();
        QVERIFY(!hash.isEmpty());
        QVERIFY(!gitIn(repo, {"update-ref", "HEAD", hash}).isNull());

        const QList<Commit> commits = log(repo);
        QCOMPARE(commits.size(), qsizetype(2));
        QCOMPARE(commits.at(0).hash, hash);
        QCOMPARE(commits.at(0).subject, QString::fromLatin1(subject));
        QCOMPARE(commits.at(0).author, QString::fromLatin1(author));
        QCOMPARE(commits.at(0).parents, QStringList{parent});
        QCOMPARE(commits.at(1).hash, parent);
        const QList<Branch> all = branches(repo);
        QCOMPARE(all.size(), qsizetype(1));
        QCOMPARE(all.first().subject, QString::fromLatin1(subject));
        QVERIFY(all.first().current);
    }

    // ------------------------------------------------------------------
    // Where a repository is.

    void aFolderCalledDotGitIsNoRepository()
    {
        const QString repo = makeRepo();
        QVERIFY(QDir().mkpath(repo + "/sub/.git"));
        QVERIFY(write(repo + "/sub/new.txt", "new\n"));
        // (git goes on up, as it does: the paths are the repository's.)
        QCOMPARE(topLevel(repo + "/sub"), repo);
        QCOMPARE(topLevel(repo + "/sub/new.txt"), repo);
        const Repository r = read(repo + "/sub");
        QVERIFY(r.valid);
        QCOMPARE(r.root, repo);
        // Nowhere near one: nothing.
        const QString lonely = folder("lonely");
        QVERIFY(QDir().mkpath(lonely + "/.git"));
        QCOMPARE(topLevel(lonely), QString());
        QVERIFY(!read(lonely).valid);
        // A .git file: a worktree's, a submodule's.
        const QString worktree = folder("worktrees") + "/wt";
        QVERIFY(!gitIn(repo, {"worktree", "add", "-q", worktree}).isNull());
        QCOMPARE(topLevel(worktree + "/a.txt"), worktree);
    }

    void aRepositoryInEveryState()
    {
        // Before the first commit.
        const QString fresh = folder("fresh");
        QVERIFY(init(fresh).ok());
        QVERIFY(write(fresh + "/new.txt", "new\n"));
        Repository r = read(fresh);
        QVERIFY(r.valid);
        QVERIFY(r.head.isEmpty());
        QCOMPARE(r.branch, QStringLiteral("main"));
        QVERIFY(r.branchText().contains("main"));
        QVERIFY(diff(fresh).contains("+new"));
        QVERIFY(log(fresh).isEmpty());
        QVERIFY(stage(fresh, {"new.txt"}).ok());
        QVERIFY(read(fresh).stagedCount() == 1);
        QVERIFY(diff(fresh, {}, DiffOf::Staged).contains("+new"));
        QVERIFY(unstage(fresh, {"new.txt"}).ok());
        QVERIFY(read(fresh).stagedCount() == 0);
        // (Edited since it was staged: git rm --cached alone refuses that.)
        QVERIFY(stage(fresh, {"new.txt"}).ok());
        QVERIFY(write(fresh + "/new.txt", "newer\n"));
        const Result edited = unstage(fresh, {"new.txt"});
        QVERIFY2(edited.ok(), qPrintable(edited.error()));
        QCOMPARE(readAll(fresh + "/new.txt"), QByteArray("newer\n"));
        QVERIFY(status(fresh).repository);
        QVERIFY(status(fresh).dirty);

        // A file's deletion staged already, staged and committed by its name.
        const QString gone = makeRepo();
        QVERIFY(QFile::remove(gone + "/a.txt"));
        QVERIFY(stage(gone, {"a.txt"}).ok());
        QVERIFY(stage(gone, {"a.txt"}).ok());
        const Result deleted = commit(gone, "gone", false, {"a.txt"});
        QVERIFY2(deleted.ok(), qPrintable(deleted.error()));
        QVERIFY(!stage(gone, {"never-was.txt"}).ok() || read(gone).changedCount() == 0);

        // Detached.
        const QString repo = makeRepo();
        QVERIFY(!gitIn(repo, {"tag", "v1"}).isNull());
        QVERIFY(checkOutCommit(repo, "v1").ok());
        r = read(repo);
        QVERIFY(r.branch.isEmpty());
        QVERIFY(!r.head.isEmpty());
        QVERIFY(r.branchText().contains(r.head));
        QVERIFY(pushArgs(r).isEmpty());
        const Status detached = status(repo);
        QVERIFY(detached.repository);
        QVERIFY(detached.branch.isEmpty());
        QVERIFY(!log(repo).isEmpty());
        QVERIFY(!qucs_s::git::history(repo, HistoryQuery()).isEmpty());
        QVERIFY(switchTo(repo, "main").ok());

        // A branch with no commit yet where others have them: every branch's
        // history is still theirs.
        QVERIFY(!gitIn(repo, {"checkout", "-q", "--orphan", "new"}).isNull());
        QVERIFY(qucs_s::git::history(repo, HistoryQuery()).size() == 1);
        QVERIFY(switchTo(repo, "main").ok());

        // Linked worktree: its own HEAD, its own operation, what it commits.
        const QString worktree = folder("worktrees") + "/wt";
        QVERIFY(!gitIn(repo, {"worktree", "add", "-q", "-b", "side", worktree}).isNull());
        r = read(worktree);
        QVERIFY(r.valid);
        QCOMPARE(r.branch, QStringLiteral("side"));
        QCOMPARE(r.root, worktree);
        QVERIFY(r.gitDir.contains("worktrees"));
        QVERIFY(write(worktree + "/w.txt", "w\n"));
        QVERIFY(commit(worktree, "in the worktree", false, {"w.txt"}).ok());
        QVERIFY(log(repo, {}, 10, "side").first().subject == QLatin1String("in the worktree"));

        // Shallow clone.
        const QString origin = makeRepo("origin");
        QVERIFY(write(origin + "/a.txt", "second\n"));
        QVERIFY(!gitIn(origin, {"commit", "-q", "-a", "-m", "second"}).isNull());
        const QString shallow = folder("shallow") + "/c";
        QVERIFY(!gitIn(dir.path(), {"clone", "-q", "--depth", "1", "file://" + origin, shallow}).isNull());
        QVERIFY(read(shallow).valid);
        QCOMPARE(log(shallow).size(), qsizetype(1));
        QVERIFY(!commitDiff(shallow, "HEAD").isEmpty());
        QVERIFY(status(shallow).repository);

        // Bare: no work tree, nothing to show.
        const QString bare = folder("bare") + "/b.git";
        QVERIFY(!gitIn(dir.path(), {"init", "-q", "--bare", bare}).isNull());
        QCOMPARE(topLevel(bare), QString());
        QVERIFY(!read(bare).valid);
        QVERIFY(!status(bare).repository);

        // A .git folder that is a bare repository.
        const QString oddBare = folder("bare");
        QVERIFY(!gitIn(dir.path(), {"init", "-q", "--bare", oddBare + "/.git"}).isNull());
        QVERIFY(!read(oddBare).valid);

        // A .git file that leads nowhere.
        const QString broken = folder("broken");
        QVERIFY(write(broken + "/.git", "gitdir: /no/such/place\n"));
        QVERIFY(!read(broken).valid);
        QVERIFY(!status(broken).repository);

        // The index locked by another: looked at, and said when changed.
        const QString locked = makeRepo();
        QVERIFY(write(locked + "/.git/index.lock", ""));
        QVERIFY(write(locked + "/a.txt", "changed\n"));
        QVERIFY(read(locked).valid);
        const Result refused = stage(locked, {"a.txt"});
        QVERIFY(!refused.ok());
        QVERIFY2(refused.error().contains("index.lock"), qPrintable(refused.error()));
        QFile::remove(locked + "/.git/index.lock");

        // A corrupt index: no state, no crash; a command says so.
        const QString corrupt = makeRepo();
        QVERIFY(write(corrupt + "/.git/index", "this is no index"));
        QVERIFY(!read(corrupt).valid);
        QVERIFY(!stage(corrupt, {"a.txt"}).ok());
        QVERIFY(!stage(corrupt, {"a.txt"}).error().isEmpty());
        QVERIFY(diff(corrupt).contains("index") || diff(corrupt).isEmpty());
        // A corrupt HEAD.
        const QString headless = makeRepo();
        QVERIFY(write(headless + "/.git/HEAD", "garbage\n"));
        QVERIFY(!read(headless).valid);
        QVERIFY(log(headless).isEmpty());
        QVERIFY(branches(headless).isEmpty());
    }

    void whatIsUnderWayIsSaidRightly()
    {
        // git am: its own abort (rebase --abort refuses it).
        const QString repo = makeRepo();
        QVERIFY(!gitIn(repo, {"checkout", "-q", "-b", "t"}).isNull());
        QVERIFY(write(repo + "/a.txt", "theirs\n"));
        QVERIFY(!gitIn(repo, {"commit", "-q", "-a", "-m", "theirs"}).isNull());
        const QString patches = folder("patches");
        QVERIFY(!gitIn(repo, {"format-patch", "-q", "-1", "-o", patches}).isNull());
        QVERIFY(!gitIn(repo, {"checkout", "-q", "main"}).isNull());
        QVERIFY(write(repo + "/a.txt", "ours\n"));
        QVERIFY(!gitIn(repo, {"commit", "-q", "-a", "-m", "ours"}).isNull());
        const QString patch = patches + "/" + QDir(patches).entryList(QDir::Files).first();
        QVERIFY(!run(repo, {"am", patch}).ok());
        QCOMPARE(read(repo).operation, QStringLiteral("am"));
        const Result aborted = abort(repo, "am");
        QVERIFY2(aborted.ok(), qPrintable(aborted.error()));
        QCOMPARE(read(repo).operation, QString());

        // A rebase stopped at a merge: MERGE_HEAD is there too, and it is the
        // rebase that is given up.
        const QString rebasing = makeRepo();
        QVERIFY(!gitIn(rebasing, {"checkout", "-q", "-b", "x"}).isNull());
        QVERIFY(write(rebasing + "/a.txt", "x\n"));
        QVERIFY(!gitIn(rebasing, {"commit", "-q", "-a", "-m", "x"}).isNull());
        QVERIFY(!gitIn(rebasing, {"checkout", "-q", "-b", "y", "main"}).isNull());
        QVERIFY(write(rebasing + "/a.txt", "y\n"));
        QVERIFY(!gitIn(rebasing, {"commit", "-q", "-a", "-m", "y"}).isNull());
        QVERIFY(!run(rebasing, {"merge", "--no-edit", "x"}).ok());
        QVERIFY(write(rebasing + "/a.txt", "both\n"));
        QVERIFY(!gitIn(rebasing, {"commit", "-q", "-a", "-m", "the merge"}).isNull());
        QVERIFY(!gitIn(rebasing, {"checkout", "-q", "main"}).isNull());
        QVERIFY(write(rebasing + "/m.txt", "m\n"));
        QVERIFY(!gitIn(rebasing, {"add", "-A"}).isNull());
        QVERIFY(!gitIn(rebasing, {"commit", "-q", "-m", "m"}).isNull());
        QVERIFY(!gitIn(rebasing, {"checkout", "-q", "y"}).isNull());
        const QByteArray tip = gitIn(rebasing, {"rev-parse", "HEAD"});
        QVERIFY(!run(rebasing, {"rebase", "--rebase-merges", "main"}).ok());
        QVERIFY(QFileInfo::exists(rebasing + "/.git/MERGE_HEAD"));
        const Repository r = read(rebasing);
        QCOMPARE(r.operation, QStringLiteral("rebase"));
        QVERIFY(abort(rebasing, r.operation).ok());
        QCOMPARE(read(rebasing).operation, QString());
        QCOMPARE(gitIn(rebasing, {"rev-parse", "HEAD"}), tip);
    }

    // ------------------------------------------------------------------
    // Names in git's output.

    void untrackedFilesWithAnyNameAreShown()
    {
        const QString repo = makeRepo();
        const QHash<QString, QByteArray> files{{QStringLiteral("caf\u00e9.txt"), "+accent line"},
                                               {QStringLiteral("\u0645\u06cc\u062b\u0645.txt"), "+persian line"},
                                               {QStringLiteral("quo\"te.txt"), "+quote line"},
                                               {QStringLiteral("ta\tb.txt"), "+tab line"},
                                               {QStringLiteral("new\nline.txt"), "+newline line"},
                                               {QStringLiteral(" spaces .txt"), "+spaces line"},
                                               {QStringLiteral("-dash.txt"), "+dash line"}};
        for (auto it = files.cbegin(); it != files.cend(); ++it) QVERIFY(write(repo + "/" + it.key(), it.value().mid(1) + "\n"));
        const QString all = diff(repo);
        for (auto it = files.cbegin(); it != files.cend(); ++it) QVERIFY2(all.contains(QString::fromUtf8(it.value())), it.value().constData());
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QVERIFY(diff(repo, it.key()).contains(QString::fromUtf8(it.value())));
            QVERIFY(entry(read(repo), it.key()) != nullptr);
        }
        const Status s = status(repo);
        QCOMPARE(s.changes.size(), qsizetype(files.size()));
        for (const FileChange& f : s.changes) QCOMPARE(f.added, 1);
        QVERIFY(stage(repo, {}).ok());
        QVERIFY(commit(repo, "names").ok());
        // Each in its commit's files, its lines counted.
        const QList<ChangedFile> changed = changedFiles(repo, "HEAD");
        QCOMPARE(changed.size(), qsizetype(files.size()));
        for (const ChangedFile& f : changed) {
            QVERIFY2(files.contains(f.path), qPrintable(f.path));
            QCOMPARE(f.added, 1);
            bool ok = false;
            QCOMPARE(fileAt(repo, "HEAD", f.path, &ok), files.value(f.path).mid(1) + "\n");
            QVERIFY(ok);
            QCOMPARE(blame(repo, f.path).size(), qsizetype(1));
        }
    }

    // A new file as git's diff shows it, read here: its header, its lines, its
    // missing last line end, a binary one, a link, a file that is too much.
    void aNewFilesDiffIsAsGitWritesIt()
    {
        const QString repo = makeRepo();
        QVERIFY(write(repo + "/two.txt", "one\ntwo\n"));
        QVERIFY(write(repo + "/noend.txt", "one\ntwo"));
        QVERIFY(write(repo + "/empty.txt", ""));
        QVERIFY(write(repo + "/bin.dat", QByteArray("a\0b", 3)));
        QVERIFY(write(repo + "/run.sh", "#!/bin/sh\n"));
        QVERIFY(QFile::setPermissions(repo + "/run.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::link("two.txt", repo + "/link"));
        // git's own, in the lines that say something.
        const auto gits = [&](const QString& name) {
            QProcess p;
            p.setWorkingDirectory(repo);
            p.start(QStringLiteral("git"), {"diff", "--no-index", "--no-color", "--", "/dev/null", name});
            p.waitForFinished(30000);
            QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'));
            lines.removeIf([](const QString& l) { return l.startsWith(QLatin1String("index ")); });
            return lines.join(QLatin1Char('\n'));
        };
        for (const QString& name : {QStringLiteral("two.txt"), QStringLiteral("noend.txt"), QStringLiteral("empty.txt"), QStringLiteral("bin.dat"), QStringLiteral("run.sh")})
            QCOMPARE(diff(repo, name), gits(name));
        // (A link: what it points at, a line; Qt says it from the top.)
        QVERIFY(diff(repo, "link").contains("new file mode 120000"));
        QVERIFY(diff(repo, "link").contains("two.txt"));

        // More new files than are shown: said.
        for (int i = 0; i < 205; ++i) QVERIFY(write(repo + QStringLiteral("/many/f%1.txt").arg(i), "x\n"));
        const QString all = diff(repo, "many");
        QCOMPARE(all.count(QStringLiteral("diff --git")), 200);
        QVERIFY(all.contains(QStringLiteral("5 more new files")));
    }

    void aRenamesNamesAndLinesAreRead()
    {
        const QString repo = makeRepo();
        QVERIFY(write(repo + "/from\tname.txt", "1\n2\n3\n4\n5\n6\n7\n8\n"));
        QVERIFY(stage(repo, {}).ok());
        QVERIFY(commit(repo, "one").ok());
        QVERIFY(!gitIn(repo, {"mv", "from\tname.txt", "to\tname.txt"}).isNull());
        QVERIFY(write(repo + "/to\tname.txt", "1\n2\n3\n4\n5\n6\n7\n8\n9\n"));
        QVERIFY(stage(repo, {}).ok());
        const Repository r = read(repo);
        QVERIFY(entry(r, "to\tname.txt") != nullptr);
        QCOMPARE(entry(r, "to\tname.txt")->from, QStringLiteral("from\tname.txt"));
        QVERIFY(commit(repo, "moved").ok());
        const QList<ChangedFile> changed = changedFiles(repo, "HEAD");
        QCOMPARE(changed.size(), qsizetype(1));
        QCOMPARE(changed.first().status, QLatin1Char('R'));
        QCOMPARE(changed.first().from, QStringLiteral("from\tname.txt"));
        QCOMPARE(changed.first().path, QStringLiteral("to\tname.txt"));
        QCOMPARE(changed.first().added, 1);
    }

    // A name Qt gives with an accent composed, on a file whose name is
    // decomposed on the disk, or the other way: its state is found.
    void anAccentIsTheSameInEitherForm()
    {
        const QString repo = makeRepo();
#ifndef Q_OS_MACOS
        QSKIP("two names on other systems' disks");
#endif
        const QString composed = QStringLiteral("caf\u00e9.txt");
        const QString decomposed = QStringLiteral("cafe\u0301.txt");
        QVERIFY(write(repo + "/" + decomposed, "new\n"));
        const Repository r = read(repo);
        QCOMPARE(r.changedCount(), 1);
        for (const QString& name : QDir(repo).entryList(QDir::Files)) {
            if (!name.startsWith(QLatin1String("caf"))) continue;
            QVERIFY2(r.entryOf(relativePath(repo, repo + "/" + name)) != nullptr, qPrintable(r.entries.first().path));
        }
        QVERIFY(r.entryOf(composed) != nullptr);
        QVERIFY(r.entryOf(decomposed) != nullptr);
        // A line in .gitignore, given either form.
        QVERIFY(ignore(repo, decomposed));
        QVERIFY(read(repo).entryOf(composed) != nullptr && read(repo).entryOf(composed)->ignored);
        // Discarded by either: said done, it is done.
        QVERIFY(QFile::remove(repo + "/.gitignore"));
        QVERIFY(discard(repo, {decomposed}).ok());
        QVERIFY(!QFileInfo::exists(repo + "/" + decomposed));
    }

    void remotesOfAPartialClone()
    {
        const QString repo = makeRepo();
        QVERIFY(!gitIn(repo, {"remote", "add", "origin", "https://example.invalid/x y.git"}).isNull());
        QVERIFY(!gitIn(repo, {"config", "remote.origin.partialclonefilter", "blob:none"}).isNull());
        const QList<Remote> all = remotes(repo);
        QCOMPARE(all.size(), qsizetype(1));
        QCOMPARE(all.first().fetchUrl, QStringLiteral("https://example.invalid/x y.git"));
        QCOMPARE(all.first().pushUrl, QStringLiteral("https://example.invalid/x y.git"));
    }

    void colourSettingsLeaveTheTextsPlain()
    {
        const QString repo = makeRepo();
        QVERIFY(!gitIn(repo, {"config", "color.ui", "always"}).isNull());
        QVERIFY(write(repo + "/a.txt", "changed\n"));
        QVERIFY(!gitIn(repo, {"stash", "push", "-q"}).isNull());
        QVERIFY(write(repo + "/a.txt", "changed again\n"));
        const QChar escape(0x1b);
        QVERIFY(!show(repo, "HEAD").contains(escape));
        QVERIFY(!showStash(repo, 0).contains(escape));
        QVERIFY(!commitDiff(repo, "HEAD").contains(escape));
        QVERIFY(!diff(repo).contains(escape));
        QVERIFY(!run(repo, {"log", "--graph", "--format=%h%d %s"}, true).out.contains(escape));
        QVERIFY(!run(repo, {"commit", "-a", "-m", "with colours", "--stat"}).out.contains(escape));
    }

    void stashesAreNumbered()
    {
        const QString repo = makeRepo();
        QVERIFY(!gitIn(repo, {"config", "log.date", "iso"}).isNull());
        for (const char* text : {"one\n", "two\n"}) {
            QVERIFY(write(repo + "/a.txt", text));
            QVERIFY(stash(repo, text, false).ok());
        }
        const QList<Stash> all = stashes(repo);
        QCOMPARE(all.size(), qsizetype(2));
        QCOMPARE(all.at(0).index, 0);
        QCOMPARE(all.at(1).index, 1);
    }

    // ------------------------------------------------------------------
    // A ref's name is not an option.

    void aNameFromTheRepositoryIsNoOption()
    {
        const QString repo = makeRepo();
        // A branch git's porcelain would refuse, made with its plumbing.
        QVERIFY(!gitIn(repo, {"update-ref", "refs/heads/--force", "HEAD"}).isNull());
        QVERIFY(!gitIn(repo, {"remote", "add", "origin", "https://example.invalid/x.git"}).isNull());
        QVERIFY(!gitIn(repo, {"symbolic-ref", "HEAD", "refs/heads/--force"}).isNull());
        Repository r = read(repo);
        QCOMPARE(r.branch, QStringLiteral("--force"));
        QString why;
        QVERIFY(pushArgs(r, &why).isEmpty());
        QVERIFY(!why.isEmpty());
        // A remote's.
        QVERIFY(!gitIn(repo, {"symbolic-ref", "HEAD", "refs/heads/main"}).isNull());
        QVERIFY(!gitIn(repo, {"remote", "remove", "origin"}).isNull());
        QVERIFY(write(repo + "/.git/config", readAll(repo + "/.git/config") + "[remote \"--receive-pack=evil\"]\n\turl = https://example.invalid/x.git\n"));
        r = read(repo);
        QCOMPARE(remotes(repo).first().name, QStringLiteral("--receive-pack=evil"));
        why.clear();
        QVERIFY(pushArgs(r, &why).isEmpty());
        QVERIFY(!why.isEmpty());

        // Given to gh.
        const QString marks = folder("marks");
        const QString gh = marks + "/gh.sh";
        QVERIFY(write(gh, "#!/bin/sh\necho \"$@\" >> '" + QFile::encodeName(marks + "/called") + "'\nexit 1\n"));
        QVERIFY(QFile::setPermissions(gh, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("QUCS_GH", QFile::encodeName(gh));
        QCOMPARE(pullRequest(repo, "--web").number, 0);
        qunsetenv("QUCS_GH");
        QVERIFY2(!QFileInfo::exists(marks + "/called"), readAll(marks + "/called").constData());
    }

    // ------------------------------------------------------------------
    // .gitignore: a line for the file alone.

    void aLineInGitignoreIsForTheFileAlone()
    {
        const QString repo = makeRepo();
        const auto ignored = [&](const QString& path) { return !gitIn(repo, {"check-ignore", "-q", "--no-index", "--", path}).isNull(); };
        QVERIFY(write(repo + "/sp ", "x\n"));
        QVERIFY(write(repo + "/sp", "x\n"));
        QVERIFY(ignore(repo, "sp "));
        QVERIFY(ignored("sp "));
        QVERIFY(!ignored("sp"));
        QVERIFY(write(repo + "/#hash", "x\n"));
        QVERIFY(ignore(repo, "#hash"));
        QVERIFY(ignored("#hash"));
        QVERIFY(write(repo + "/!bang", "x\n"));
        QVERIFY(ignore(repo, "!bang"));
        QVERIFY(ignored("!bang"));
        QVERIFY(write(repo + "/st*r", "x\n"));
        QVERIFY(write(repo + "/star", "x\n"));
        QVERIFY(ignore(repo, "st*r"));
        QVERIFY(ignored("st*r"));
        QVERIFY(!ignored("star"));

        // A name with a line in it would be two lines: a pattern of its own.
        QVERIFY(write(repo + "/x\n*", "x\n"));
        QString why;
        QVERIFY(!ignore(repo, "x\n*", &why));
        QVERIFY(!why.isEmpty());
        QVERIFY(!ignored("a.txt"));
        // Not in the repository.
        QVERIFY(!ignore(repo, "../elsewhere", &why));
        QVERIFY(!readAll(repo + "/.gitignore").contains(".."));
    }

    // ------------------------------------------------------------------
    // What changed in a schematic, part by part.

    void aSchematicOfAnyNameAndAMergesAreSummed()
    {
        const QString repo = makeRepo();
        const auto schematic = [](const char* value) {
            return QByteArray("<Qucs Schematic 26.1.1>\n<Components>\n  <R R1 1 100 100 15 -26 0 1 \"") + value
                   + "\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n</Components>\n";
        };
        const QString name = QStringLiteral("\u0645\u062f\u0627\u0631 caf\u00e9.sch");
        QVERIFY(write(repo + "/" + name, schematic("1 kOhm")));
        QVERIFY(stage(repo, {}).ok());
        QVERIFY(commit(repo, "the schematic").ok());
        QVERIFY(write(repo + "/" + name, schematic("2 kOhm")));
        QVERIFY(commit(repo, "changed", false, {name}).ok());
        const QString parts = schematicChangesIn(repo, "HEAD");
        QVERIFY2(parts.contains(name), qPrintable(parts));
        QVERIFY(parts.contains("R1"));

        // A merge: what it brought into its branch.
        QVERIFY(!gitIn(repo, {"checkout", "-q", "-b", "side", "HEAD~1"}).isNull());
        QVERIFY(write(repo + "/side.txt", "x\n"));
        QVERIFY(stage(repo, {}).ok());
        QVERIFY(commit(repo, "side").ok());
        QVERIFY(!gitIn(repo, {"checkout", "-q", "main"}).isNull());
        QVERIFY(!gitIn(repo, {"checkout", "-q", "-b", "merged", "HEAD~1"}).isNull());
        QVERIFY(!gitIn(repo, {"merge", "-q", "--no-ff", "--no-edit", "main"}).isNull());
        QVERIFY(schematicChangesIn(repo, "HEAD").contains("R1"));

        // A link with a schematic's name: not read.
        QVERIFY(QFile::link("/dev/zero", repo + "/zero.sch"));
        QVERIFY(cameBack([repo] { schematicChangesOf(repo); }));
    }

    // ------------------------------------------------------------------
    // The versions of a conflict: their bytes.

    void bothVersionsAreWrittenByteForByte()
    {
        const QString repo = makeRepo();
        const QByteArray ours("r\xb5m\xe9 ours \0 x\n", 15), theirs("r\xb5m\xe9 theirs\n");
        QVERIFY(makeConflict(repo, {{"old.txt", {ours, theirs}}}));
        const QStringList written = Commands::instance()->openConflictVersions(repo, repo + "/old.txt");
        QCOMPARE(written.size(), qsizetype(2));
        QCOMPARE(readAll(written.at(0)), ours);
        QCOMPARE(readAll(written.at(1)), theirs);
    }

    // ------------------------------------------------------------------
    // The tracker: an older read does not replace a newer one.

    void anOldReadDoesNotReplaceANewOne()
    {
        const QString repo = makeRepo();
        const QString work = folder("shim");
        const QString shim = work + "/git.sh";
        // (status: its answer taken, then held back while the flag is there.)
        QVERIFY(write(shim, "#!/bin/sh\nfor a in \"$@\"; do\n  if [ \"$a\" = status ] && [ -f '" + QFile::encodeName(work)
                                + "/slow' ]; then\n    '" + QFile::encodeName(QStandardPaths::findExecutable("git")) + "' \"$@\" > '"
                                + QFile::encodeName(work) + "/held'\n    touch '" + QFile::encodeName(work) + "/snapped'\n    sleep 2\n    cat '"
                                + QFile::encodeName(work) + "/held'\n    exit 0\n  fi\ndone\nexec '"
                                + QFile::encodeName(QStandardPaths::findExecutable("git")) + "' \"$@\"\n"));
        QVERIFY(QFile::setPermissions(shim, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("QUCS_GIT", QFile::encodeName(shim));
        Tracker* tracker = Tracker::instance();
        QVERIFY(tracker->readNow(repo) != nullptr);
        QVERIFY(write(work + "/slow", ""));
        tracker->refresh(repo);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(work + "/snapped"), 10000);
        // That read has its answer. Now a commit's change, read at once.
        QVERIFY(write(repo + "/new.txt", "new\n"));
        QFile::remove(work + "/slow");
        const Repository* now = tracker->readNow(repo);
        QVERIFY(now != nullptr && now->entryOf("new.txt") != nullptr);
        QSignalSpy changed(tracker, &Tracker::changed);
        QTRY_VERIFY_WITH_TIMEOUT(!tracker->isReading(), 10000);
        QTest::qWait(300);
        const Repository* after = tracker->repositoryOf(repo);
        qunsetenv("QUCS_GIT");
        QVERIFY(after != nullptr);
        QVERIFY2(after->entryOf("new.txt") != nullptr, "the older read replaced the newer one");
    }
};

QTEST_MAIN(TestGitRobust)
#include "test_git_robust.moc"
