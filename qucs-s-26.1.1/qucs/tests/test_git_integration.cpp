/*
 * Git in Qucs-S, as Eclipse has it: a repository read (what is staged and
 * what is not, the untracked and the ignored, a rename, a conflict, the
 * branch and its upstream, an operation under way), what is done with it
 * (stage, unstage, discard to the trash, commit and amend, untrack, ignore,
 * branches, merges, tags, stashes, the history, blame, remotes - a push,
 * fetch, pull, clone through a bare repository here); the File Browser's
 * colours, letters and Git menu, the Git menu left of Help, the status
 * bar's chip and its menu, the commit, history and blame windows; and
 * Claude's git tools. Names and refs git would take for options, and paths
 * out of the repository, are refused.
 */
#include <QtTest>
#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "filebrowser.h"
#include "gitrepo.h"
#include "gitstatus.h"
#include "gitui.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "components/component.h"
#include "mouseactions.h"
#include "qucsdoc.h"
#include "schematic.h"
#include "schematicdiff.h"
#include "settings.h"
#include "textdoc.h"

using namespace qucs_s::git;

namespace {

constexpr int GitLetterRole = Qt::UserRole + 40;   // (filebrowser.cpp's)
constexpr int GitBranchRole = Qt::UserRole + 41;

bool write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

// Written as git writes a file: a new one put in its place (a watch on
// the old one ends).
bool replaceFile(const QString& path, const QByteArray& bytes)
{
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// git in \a dir; its output, or a null string when it failed.
QString gitIn(const QString& dir, const QStringList& args)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(program(), args);
    if (!p.waitForFinished(30000) || p.exitCode() != 0) {
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

QMenu* submenuNamed(QMenu* menu, const QString& name)
{
    for (QAction* a : menu->actions())
        if (a->menu() != nullptr && a->menu()->objectName() == name) return a->menu();
    return nullptr;
}

// Answers the next message box named \a name with its button \a button.
void answerBox(const QString& name, const QString& button)
{
    auto* timer = new QTimer(qApp);
    auto tries = std::make_shared<int>(0);
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, [timer, tries, name, button] {
        if (++*tries > 500) {
            timer->deleteLater();
            return;
        }
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (box == nullptr || !box->isVisible() || box->objectName() != name) continue;
            for (QAbstractButton* b : box->buttons())
                if (b->text().remove(QLatin1Char('&')) == button) {
                    timer->stop();
                    timer->deleteLater();
                    b->click();
                    return;
                }
        }
    });
    timer->start();
}

// Types \a text into the next input dialog and accepts it.
void answerInput(const QString& text)
{
    auto* timer = new QTimer(qApp);
    auto tries = std::make_shared<int>(0);
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, [timer, tries, text] {
        if (++*tries > 500) {
            timer->deleteLater();
            return;
        }
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* d = qobject_cast<QInputDialog*>(w);
            if (d == nullptr || !d->isVisible()) continue;
            timer->stop();
            timer->deleteLater();
            if (d->comboBoxItems().isEmpty()) d->setTextValue(text);
            else d->setTextValue(text);
            d->accept();
            return;
        }
    });
    timer->start();
}

// The message boxes that come up while it lives: their texts, each
// answered with \a button (else closed).
class BoxCounter
{
public:
    /// (Boxes named in \a others are left to their own answerers.)
    explicit BoxCounter(const QString& button = QStringLiteral("No"), const QStringList& others = {})
        : a_button(button), a_others(others)
    {
        a_timer.setInterval(20);
        QObject::connect(&a_timer, &QTimer::timeout, [this] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(w);
                if (box == nullptr || !box->isVisible() || a_answered.contains(box) || a_others.contains(box->objectName())) continue;
                a_answered << box;
                a_texts << box->text();
                QAbstractButton* chosen = nullptr;
                for (QAbstractButton* b : box->buttons())
                    if (b->text().remove(QLatin1Char('&')) == a_button) chosen = b;
                if (chosen != nullptr) chosen->click();
                else box->reject();
            }
        });
        a_timer.start();
    }
    QStringList texts() const { return a_texts; }
    /// Those so far, forgotten.
    QStringList take() { return std::exchange(a_texts, {}); }

private:
    QString a_button;
    QStringList a_others;
    QTimer a_timer;
    QList<QPointer<QMessageBox>> a_answered;
    QStringList a_texts;
};

QStringList pathsIn(const QJsonArray& array)
{
    QStringList out;
    for (const QJsonValue& v : array) out << (v.isObject() ? v.toObject().value("path").toString() : v.toString());
    return out;
}

} // namespace

class TestGitIntegration : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;
    QString trash;

    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 60000)
    {
        return control->callNow(tool, args, timeoutMs);
    }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r)
    {
        QString first;
        for (const QJsonValue& v : r.value(QStringLiteral("content")).toArray())
            if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                first = v.toObject().value(QStringLiteral("text")).toString();
                break;
            }
        return QJsonDocument::fromJson(first.toUtf8()).object();
    }

    // A repository on main: a.txt ("one\ntwo\nthree\n") and b.txt committed.
    QString makeRepo(const QString& name)
    {
        const QString path = canonical(dir.path()) + "/" + name;
        if (!QDir().mkpath(path)) return {};
        if (gitIn(path, {"init", "-q"}).isNull()) return {};
        if (gitIn(path, {"symbolic-ref", "HEAD", "refs/heads/main"}).isNull()) return {};
        if (!write(path + "/a.txt", "one\ntwo\nthree\n") || !write(path + "/b.txt", "bee\n")) return {};
        if (gitIn(path, {"add", "."}).isNull() || gitIn(path, {"commit", "-q", "-m", "first"}).isNull()) return {};
        return path;
    }

    static const Entry* entry(const Repository& r, const QString& path)
    {
        for (const Entry& e : r.entries)
            if (e.path == path) return &e;
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        if (program().isEmpty()) QSKIP("git is not installed");
        trash = dir.filePath("trash");
        qputenv("QUCS_TRASH_DIR", QFile::encodeName(trash));
        // The tests' repositories alone, none of the user's git settings.
        qputenv("GIT_CEILING_DIRECTORIES", QFile::encodeName(canonical(dir.path())) + QByteArray(1, QDir::listSeparator().toLatin1())
                                               + QFile::encodeName(dir.path()));
        QVERIFY(write(dir.filePath("gitconfig"),
                      "[user]\n\tname = Tester\n\temail = tester@example.com\n[init]\n\tdefaultBranch = main\n"
                      "[commit]\n\tgpgsign = false\n[tag]\n\tgpgsign = false\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(dir.filePath("gitconfig")));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(canonical(dir.filePath("workspace")));
        QucsSettings.QucsWorkDir.setPath(canonical(dir.filePath("workspace")));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
    }

    void cleanupTestCase()
    {
        if (app == nullptr) return;
        for (const QPointer<QDialog>& d : Commands::instance()->openWindows())
            if (!d.isNull()) d->close();
        for (QucsDoc* doc : app->allDocuments()) doc->setDocChanged(false);
        app->closeAllFiles();
        delete app;
        QucsMain = nullptr;
    }

    // What a repository stands at: the staged, the not staged, a rename,
    // the untracked (a folder whole), the ignored; the folders with
    // changes in them; the branch.
    void aRepositoryIsRead()
    {
        const QString repo = makeRepo("read");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/.gitignore", "build/\n"));
        QVERIFY(!gitIn(repo, {"add", ".gitignore"}).isNull());
        QVERIFY(!gitIn(repo, {"commit", "-q", "-m", "ignore"}).isNull());
        QVERIFY(write(repo + "/a.txt", "one\nTWO\nthree\n"));          // modified, not staged
        QVERIFY(write(repo + "/c.txt", "sea\n"));
        QVERIFY(!gitIn(repo, {"add", "c.txt"}).isNull());               // added
        QVERIFY(!gitIn(repo, {"mv", "b.txt", "b2.txt"}).isNull());      // renamed
        QVERIFY(write(repo + "/d.txt", "dee\n"));                       // untracked
        QVERIFY(write(repo + "/newdir/x.txt", "x\n"));                  // an untracked folder
        QVERIFY(write(repo + "/build/out.dat", "0\n"));                 // ignored

        const Repository r = read(repo + "/newdir");   // (from a folder inside it)
        QVERIFY(r.valid);
        QCOMPARE(canonical(r.root), repo);
        QCOMPARE(r.branch, QStringLiteral("main"));
        QCOMPARE(r.head.size(), 7);
        QVERIFY(r.upstream.isEmpty());
        QVERIFY(r.operation.isEmpty());
        QCOMPARE(r.branchText(), QStringLiteral("main"));

        const Entry* a = entry(r, "a.txt");
        QVERIFY(a != nullptr);
        QCOMPARE(a->unstaged, QChar('M'));
        QVERIFY(a->hasUnstaged() && !a->hasStaged());
        QCOMPARE(a->letter(), QStringLiteral("M"));
        QCOMPARE(a->describe(), QStringLiteral("Modified, not staged"));
        const Entry* c = entry(r, "c.txt");
        QVERIFY(c != nullptr && c->hasStaged() && !c->hasUnstaged());
        QCOMPARE(c->letter(), QStringLiteral("A"));
        QCOMPARE(c->describe(), QStringLiteral("Added, staged"));
        const Entry* b2 = entry(r, "b2.txt");
        QVERIFY(b2 != nullptr);
        QCOMPARE(b2->staged, QChar('R'));
        QCOMPARE(b2->from, QStringLiteral("b.txt"));
        QVERIFY(entry(r, "d.txt") != nullptr && entry(r, "d.txt")->untracked);
        QCOMPARE(entry(r, "d.txt")->letter(), QStringLiteral("U"));
        const Entry* folder = entry(r, "newdir/");
        QVERIFY(folder != nullptr && folder->isFolder() && folder->untracked);
        QCOMPARE(folder->describe(), QStringLiteral("Untracked folder"));
        QCOMPARE(r.entryOf("newdir/x.txt"), folder);   // in it: its entry
        const Entry* build = r.entryOf("build/out.dat");
        QVERIFY(build != nullptr && build->ignored);
        QCOMPARE(build->letter(), QStringLiteral("I"));
        QVERIFY(r.entryOf(".gitignore") == nullptr);   // as committed
        QVERIFY(r.changedIn("") && r.changedIn("newdir") && !r.changedIn("build"));
        QCOMPARE(r.changedCount(), 5);   // a, c, b2, d, newdir/ (the ignored not)
        QCOMPARE(r.stagedCount(), 2);
        QCOMPARE(r.conflictCount(), 0);

        // Outside one: nothing.
        const QString plain = canonical(dir.path()) + "/plain";
        QVERIFY(QDir().mkpath(plain));
        QVERIFY(topLevel(plain).isEmpty());
        QVERIFY(!read(plain).valid);
        // Before the first commit.
        const QString fresh = canonical(dir.path()) + "/fresh";
        QVERIFY(QDir().mkpath(fresh));
        QVERIFY(init(fresh).ok());
        const Repository f = read(fresh);
        QVERIFY(f.valid && f.head.isEmpty());
        QCOMPARE(f.branchText(), QStringLiteral("main (no commit yet)"));
        bool inside = false;
        QCOMPARE(relativePath(repo, repo + "/newdir/x.txt", &inside), QStringLiteral("newdir/x.txt"));
        QVERIFY(inside);
        QVERIFY(relativePath(repo, plain, &inside).isEmpty());
        QVERIFY(!inside);
    }

    // Stage, unstage, discard (what git knows back as committed, new
    // files to the trash), commit and amend, untrack, ignore - and paths
    // out of the repository refused, not taken for everything.
    void filesAreStagedCommittedAndDiscarded()
    {
        const QString repo = makeRepo("files");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/a.txt", "one\nTWO\nthree\n"));
        QVERIFY(write(repo + "/new.txt", "new\n"));
        QVERIFY(stage(repo, {repo + "/a.txt"}).ok());
        Repository r = read(repo);
        QVERIFY(entry(r, "a.txt")->hasStaged());
        QVERIFY(entry(r, "new.txt")->untracked);
        QVERIFY(unstage(repo, {repo + "/a.txt"}).ok());
        QVERIFY(!entry(read(repo), "a.txt")->hasStaged());
        QVERIFY(stage(repo, {}).ok());   // everything
        QCOMPARE(read(repo).stagedCount(), 2);
        QVERIFY(unstage(repo, {}).ok());
        QCOMPARE(read(repo).stagedCount(), 0);

        // Paths of no file of the repository: refused (none would stage all).
        const Result outside = stage(repo, {canonical(dir.path()) + "/elsewhere.txt"});
        QVERIFY(!outside.ok());
        QVERIFY2(outside.error().contains("not in the repository"), qPrintable(outside.error()));
        QCOMPARE(read(repo).stagedCount(), 0);
        QVERIFY(!unstage(repo, {canonical(dir.path()) + "/elsewhere.txt"}).ok());
        QVERIFY(!discard(repo, {canonical(dir.path()) + "/elsewhere.txt"}).ok());
        QVERIFY(!untrack(repo, {canonical(dir.path()) + "/elsewhere.txt"}).ok());
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\nTWO\nthree\n"));

        // Discard: a.txt as committed, new.txt to the trash (not deleted).
        QStringList trashed;
        QVERIFY(discard(repo, {repo + "/a.txt", repo + "/new.txt"}, &trashed).ok());
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\ntwo\nthree\n"));
        QVERIFY(!QFileInfo::exists(repo + "/new.txt"));
        QCOMPARE(trashed.size(), 1);
        QVERIFY(trashed.first().startsWith(trash));
        QCOMPARE(readAll(trashed.first()), QByteArray("new\n"));
        QCOMPARE(read(repo).changedCount(), 0);
        // A file only added: out of the index, then to the trash.
        QVERIFY(write(repo + "/added.txt", "added\n"));
        QVERIFY(stage(repo, {repo + "/added.txt"}).ok());
        QVERIFY(discard(repo, {repo + "/added.txt"}).ok());
        QVERIFY(!QFileInfo::exists(repo + "/added.txt"));
        QCOMPARE(read(repo).changedCount(), 0);

        // Commit, amend, only some paths.
        QVERIFY(write(repo + "/a.txt", "one\nTWO\nthree\n"));
        QVERIFY(write(repo + "/b.txt", "BEE\n"));
        QVERIFY(stage(repo, {repo + "/a.txt"}).ok());
        QVERIFY(commit(repo, "second\n\nwhy").ok());
        QCOMPARE(lastMessage(repo), QStringLiteral("second\n\nwhy"));
        QVERIFY(entry(read(repo), "b.txt") != nullptr);   // not staged: not in it
        QVERIFY(commit(repo, "second, amended", true).ok());
        QCOMPARE(lastMessage(repo), QStringLiteral("second, amended"));
        QCOMPARE(log(repo).size(), 2);
        QVERIFY(commit(repo, "b alone", false, {repo + "/b.txt"}).ok());
        QCOMPARE(read(repo).changedCount(), 0);
        QCOMPARE(log(repo).first().subject, QStringLiteral("b alone"));

        // Names git or a shell could misread: a leading dash, spaces,
        // accents, quotes.
        for (const QString& name : {QStringLiteral("-n.txt"), QStringLiteral("two words é.txt"), QStringLiteral("it's \"q\".txt")}) {
            QVERIFY(write(repo + "/" + name, "odd\n"));
            QVERIFY2(read(repo).entryOf(name) != nullptr && read(repo).entryOf(name)->untracked, qPrintable(name));
            QVERIFY2(stage(repo, {repo + "/" + name}).ok(), qPrintable(name));
            QVERIFY(read(repo).entryOf(name)->hasStaged());
            QVERIFY(diff(repo, repo + "/" + name, DiffOf::Staged).contains("+odd"));
            QVERIFY(unstage(repo, {repo + "/" + name}).ok());
            QVERIFY(discard(repo, {repo + "/" + name}).ok());
            QVERIFY(!QFileInfo::exists(repo + "/" + name));
        }
        QCOMPARE(read(repo).changedCount(), 0);

        // Untrack (the file stays), ignore (once).
        QVERIFY(untrack(repo, {repo + "/b.txt"}).ok());
        Repository u = read(repo);
        QCOMPARE(entry(u, "b.txt")->staged, QChar('D'));
        QVERIFY(QFileInfo::exists(repo + "/b.txt"));
        QString why;
        QVERIFY(ignore(repo, repo + "/b.txt", &why));
        QVERIFY(ignore(repo, repo + "/b.txt", &why));   // there already
        QCOMPARE(readAll(repo + "/.gitignore"), QByteArray("/b.txt\n"));
        QVERIFY(write(repo + "/odd [1].txt", "x\n"));
        QVERIFY(ignore(repo, repo + "/odd [1].txt", &why));
        QVERIFY(readAll(repo + "/.gitignore").contains("/odd \\[1\\].txt\n"));
        QVERIFY(read(repo).entryOf("odd [1].txt")->ignored);
        QVERIFY(!ignore(repo, canonical(dir.path()) + "/plain", &why));
        QVERIFY(!why.isEmpty());
    }

    // Branches (one of a remote checked out as a local one following it),
    // a merge in conflict and given up, tags, stashes.
    void branchesTagsAndStashes()
    {
        const QString repo = makeRepo("branches");
        QVERIFY(!repo.isEmpty());
        QVERIFY(createBranch(repo, "feature").ok());
        QCOMPARE(read(repo).branch, QStringLiteral("feature"));
        QVERIFY(write(repo + "/a.txt", "one\nfeature\nthree\n"));
        QVERIFY(commit(repo, "on feature", false, {repo + "/a.txt"}).ok());
        QVERIFY(switchTo(repo, "main").ok());
        QVERIFY(write(repo + "/a.txt", "one\nmain\nthree\n"));
        QVERIFY(commit(repo, "on main", false, {repo + "/a.txt"}).ok());
        const QList<Branch> all = branches(repo);
        QCOMPARE(all.size(), 2);
        QCOMPARE(all.at(0).name, QStringLiteral("feature"));
        QVERIFY(all.at(1).current && all.at(1).name == QStringLiteral("main"));
        QCOMPARE(all.at(1).subject, QStringLiteral("on main"));

        // A merge in conflict: said, under way, given up.
        const Result merged = merge(repo, "feature");
        QVERIFY(!merged.ok());
        QVERIFY2(merged.error().contains("CONFLICT"), qPrintable(merged.error()));
        Repository r = read(repo);
        QCOMPARE(r.operation, QStringLiteral("merge"));
        QCOMPARE(r.conflictCount(), 1);
        QVERIFY(entry(r, "a.txt")->conflicted);
        QCOMPARE(entry(r, "a.txt")->letter(), QStringLiteral("!"));
        QCOMPARE(entry(r, "a.txt")->describe(), QStringLiteral("In conflict"));
        QVERIFY(abort(repo, r.operation).ok());
        r = read(repo);
        QVERIFY(r.operation.isEmpty());
        QCOMPARE(r.changedCount(), 0);
        QVERIFY(!abort(repo, QString()).ok());

        // Delete: not merged, refused; forced.
        QVERIFY(renameBranch(repo, "feature", "topic").ok());
        const Result kept = deleteBranch(repo, "topic");
        QVERIFY(!kept.ok());
        QVERIFY(deleteBranch(repo, "topic", true).ok());
        QCOMPARE(branches(repo).size(), 1);

        // Names git would take for options.
        for (const Result& refused : {switchTo(repo, "--orphan=x"), createBranch(repo, "-x"), createBranch(repo, "ok", "--help"),
                                      deleteBranch(repo, "-D"), renameBranch(repo, "main", "-m"), merge(repo, "--abort"),
                                      createTag(repo, "-d"), deleteTag(repo, "-l"), addRemote(repo, "x", "--upload-pack=touch here"),
                                      removeRemote(repo, "-v"), checkOutCommit(repo, "--orphan"), revert(repo, "--quit"),
                                      cherryPick(repo, "--abort"), reset(repo, "--hard", "soft")}) {
            QVERIFY(!refused.ok());
            QVERIFY2(refused.error().contains("begins with '-'"), qPrintable(refused.error()));
        }
        QVERIFY(fetchArgs("--upload-pack=touch here").isEmpty());
        QVERIFY(log(repo, {}, 10, "--output=" + repo + "/written").isEmpty());
        QVERIFY(!QFileInfo::exists(repo + "/written"));
        QVERIFY(show(repo, "--output=" + repo + "/written").contains("begins with '-'"));
        QVERIFY(!QFileInfo::exists(repo + "/written"));
        QCOMPARE(cloneArgs("--upload-pack=x", "/tmp/y").mid(2, 2), QStringList({"--", "--upload-pack=x"}));
        QVERIFY(!reset(repo, "HEAD", "harder").ok());

        // Tags.
        QVERIFY(createTag(repo, "v1").ok());
        QVERIFY(createTag(repo, "v0", "the first", "HEAD~1").ok());
        QVERIFY(tags(repo).contains("v1") && tags(repo).contains("v0"));
        QCOMPARE(gitIn(repo, {"cat-file", "-t", "v0"}), QStringLiteral("tag"));   // annotated
        QVERIFY(deleteTag(repo, "v1").ok());
        QCOMPARE(tags(repo), QStringList({"v0"}));

        // Stashes: put aside (untracked too), shown, brought back, dropped.
        QVERIFY(write(repo + "/a.txt", "stashed\n"));
        QVERIFY(write(repo + "/loose.txt", "loose\n"));
        QVERIFY(stash(repo, "aside").ok());
        QCOMPARE(read(repo).changedCount(), 0);
        QVERIFY(!QFileInfo::exists(repo + "/loose.txt"));
        QList<Stash> s = stashes(repo);
        QCOMPARE(s.size(), 1);
        QCOMPARE(s.first().index, 0);
        QVERIFY(s.first().message.contains("aside"));
        QVERIFY(showStash(repo, 0).contains("+stashed"));
        QVERIFY(applyStash(repo, 0, false).ok());
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("stashed\n"));
        QCOMPARE(stashes(repo).size(), 1);
        QVERIFY(discard(repo, {}).ok());
        QVERIFY(applyStash(repo, 0, true).ok());   // pop
        QVERIFY(stashes(repo).isEmpty());
        QVERIFY(stash(repo, {}).ok());
        QVERIFY(dropStash(repo, 0).ok());
        QVERIFY(stashes(repo).isEmpty());
        QCOMPARE(read(repo).changedCount(), 0);
    }

    // The history (a file's through its rename), a commit, the diffs,
    // blame; revert, cherry-pick, check out, reset.
    void historyDiffsAndBlame()
    {
        const QString repo = makeRepo("history");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/a.txt", "one\ntwo\nthree\nfour\n"));
        QVERIFY(commit(repo, "four", false, {repo + "/a.txt"}).ok());
        QVERIFY(!gitIn(repo, {"mv", "a.txt", "moved.txt"}).isNull());
        QVERIFY(commit(repo, "moved").ok());
        QVERIFY(createTag(repo, "v1").ok());
        const QList<Commit> all = log(repo);
        QCOMPARE(all.size(), 3);
        QCOMPARE(all.first().subject, QStringLiteral("moved"));
        QVERIFY(all.first().refs.contains("tag: v1"));
        QVERIFY(all.first().refs.join(' ').contains("main"));
        QCOMPARE(all.first().author, QStringLiteral("Tester"));
        QCOMPARE(all.first().email, QStringLiteral("tester@example.com"));
        QVERIFY(all.first().date.isValid());
        QCOMPARE(all.first().parents.size(), 1);
        QCOMPARE(log(repo, repo + "/moved.txt").size(), 3);   // followed
        QCOMPARE(log(repo, {}, 1, {}, 1).first().subject, QStringLiteral("four"));
        QCOMPARE(log(repo, {}, 10, "HEAD~1").size(), 2);
        const QString shown = show(repo, all.at(1).hash);
        QVERIFY(shown.contains("four") && shown.contains("+four"));

        QVERIFY(write(repo + "/moved.txt", "one\nTWO\nthree\nfour\n"));
        QVERIFY(write(repo + "/fresh.txt", "fresh\n"));
        const QString changes = diff(repo);
        QVERIFY2(changes.contains("+TWO") && changes.contains("-two") && changes.contains("+fresh"), qPrintable(changes));
        QVERIFY(diff(repo, repo + "/fresh.txt").contains("+fresh"));
        QVERIFY(!diff(repo, repo + "/moved.txt").contains("fresh"));
        QVERIFY(diff(repo, {}, DiffOf::Staged).trimmed().isEmpty());
        QVERIFY(stage(repo, {repo + "/moved.txt"}).ok());
        QVERIFY(diff(repo, {}, DiffOf::Staged).contains("+TWO"));
        QVERIFY(!diff(repo, {}, DiffOf::Unstaged).contains("+TWO"));

        const QList<BlameLine> lines = blame(repo, repo + "/moved.txt");
        QCOMPARE(lines.size(), 4);
        QCOMPARE(lines.at(0).line, 1);
        QCOMPARE(lines.at(0).text, QStringLiteral("one"));
        QCOMPARE(lines.at(0).author, QStringLiteral("Tester"));
        QCOMPARE(lines.at(0).summary, QStringLiteral("first"));
        QVERIFY(lines.at(1).uncommitted);   // TWO, not committed
        QCOMPARE(lines.at(3).summary, QStringLiteral("four"));
        QVERIFY(blame(repo, repo + "/fresh.txt").isEmpty());
        QVERIFY(discard(repo, {}).ok());

        // Revert, cherry-pick, check out alone, reset.
        QVERIFY(revert(repo, all.at(1).hash).ok());
        QCOMPARE(readAll(repo + "/moved.txt"), QByteArray("one\ntwo\nthree\n"));
        QVERIFY(log(repo).first().subject.startsWith("Revert"));
        QVERIFY(createBranch(repo, "side", all.at(2).hash).ok());
        QVERIFY(cherryPick(repo, all.at(1).hash).ok());
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\ntwo\nthree\nfour\n"));
        QVERIFY(switchTo(repo, "main").ok());
        QVERIFY(checkOutCommit(repo, "v1").ok());
        Repository detached = read(repo);
        QVERIFY(detached.branch.isEmpty());
        QCOMPARE(detached.branchText(), QStringLiteral("detached at %1").arg(detached.head));
        QVERIFY(switchTo(repo, "main").ok());
        const int before = int(log(repo).size());
        QVERIFY(reset(repo, "HEAD~1", "soft").ok());
        QCOMPARE(int(log(repo).size()), before - 1);
        QVERIFY(read(repo).stagedCount() > 0);
        QVERIFY(reset(repo, "v1", "hard").ok());
        QCOMPARE(read(repo).changedCount(), 0);
        QCOMPARE(log(repo).first().subject, QStringLiteral("moved"));
    }

    // A remote here (a bare repository): pushed to (made the upstream),
    // cloned, fetched and pulled from, through Jobs as the window runs
    // them.
    void remotesPushFetchPullClone()
    {
        const QString repo = makeRepo("local");
        QVERIFY(!repo.isEmpty());
        const QString bare = canonical(dir.path()) + "/remote.git";
        QVERIFY(QDir().mkpath(bare));
        QVERIFY(!gitIn(bare, {"init", "-q", "--bare"}).isNull());
        QString why;
        QVERIFY(pushArgs(read(repo), &why).isEmpty());
        QVERIFY(why.contains("no remote"));
        QVERIFY(addRemote(repo, "origin", bare).ok());
        QCOMPARE(remotes(repo).size(), 1);
        QCOMPARE(remotes(repo).first().fetchUrl, bare);
        const QStringList first = pushArgs(read(repo), &why);
        QCOMPARE(first.mid(first.size() - 3), QStringList({"-u", "origin", "main"}));

        const auto runJob = [](const QString& in, const QStringList& args) {
            Job job(in, args);
            QSignalSpy finished(&job, &Job::finished);
            QSignalSpy progress(&job, &Job::progress);
            job.start();
            if (!finished.wait(60000)) return Result();
            return finished.first().first().value<Result>();
        };
        QVERIFY(runJob(repo, first).ok());
        Repository r = read(repo);
        QCOMPARE(r.upstream, QStringLiteral("origin/main"));
        QCOMPARE(r.ahead, 0);
        QCOMPARE(r.branchText(), QStringLiteral("main"));

        // A clone, a commit there pushed; here: behind, then pulled.
        const QString clone = canonical(dir.path()) + "/clone";
        QVERIFY(runJob(canonical(dir.path()), cloneArgs(bare, clone)).ok());
        QCOMPARE(readAll(clone + "/a.txt"), QByteArray("one\ntwo\nthree\n"));
        QVERIFY(write(clone + "/a.txt", "one\ntwo\nthree\nfrom the clone\n"));
        QVERIFY(commit(clone, "from the clone", false, {clone + "/a.txt"}).ok());
        QCOMPARE(read(clone).ahead, 1);
        QCOMPARE(read(clone).branchText(), QStringLiteral("main ↑1"));
        QVERIFY(runJob(clone, pushArgs(read(clone))).ok());
        QVERIFY(runJob(repo, fetchArgs()).ok());
        r = read(repo);
        QCOMPARE(r.behind, 1);
        QCOMPARE(r.branchText(), QStringLiteral("main ↓1"));
        QVERIFY(runJob(repo, pullArgs()).ok());
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\ntwo\nthree\nfrom the clone\n"));
        QCOMPARE(read(repo).behind, 0);

        // A remote's branch checked out as a local one following it.
        QVERIFY(createBranch(clone, "feature").ok());
        QVERIFY(runJob(clone, pushArgs(read(clone))).ok());
        QVERIFY(runJob(repo, fetchArgs("origin")).ok());
        bool remoteFeature = false;
        for (const Branch& b : branches(repo)) remoteFeature = remoteFeature || (b.remote && b.name == "origin/feature");
        QVERIFY(remoteFeature);
        QVERIFY(switchTo(repo, "origin/feature").ok());
        r = read(repo);
        QCOMPARE(r.branch, QStringLiteral("feature"));
        QCOMPARE(r.upstream, QStringLiteral("origin/feature"));
        QVERIFY(switchTo(repo, "main").ok());

        // A job stopped.
        Job slow(repo, {"-c", "alias.wait=!sleep 20", "wait"});
        QSignalSpy finished(&slow, &Job::finished);
        slow.start();
        QVERIFY(slow.isRunning());
        slow.cancel();
        QVERIFY(finished.wait(10000) || !finished.isEmpty());
        QVERIFY(!finished.first().first().value<Result>().ok());

        QVERIFY(removeRemote(repo, "origin").ok());
        QVERIFY(remotes(repo).isEmpty());
    }

    // What only looks - the state, a diff, the history, a commit, blame,
    // the File Browser's colours - runs nothing the repository's own
    // configuration names (a file system monitor, a filter, a text
    // conversion): a downloaded project's programs. And git started from a
    // hook (GIT_DIR, GIT_INDEX_FILE set) still works on each path's own.
    void readingRunsNoneOfTheRepositorysPrograms()
    {
#ifdef Q_OS_WIN
        QSKIP("the programs here are shell scripts");
#endif
        const QString repo = makeRepo("evil");
        QVERIFY(!repo.isEmpty());
        const QString marks = canonical(dir.path()) + "/evil-marks";
        QVERIFY(QDir().mkpath(marks));
        const auto program = [&](const QString& name) {
            const QString path = canonical(dir.path()) + "/evil-programs/" + name + ".sh";
            write(path, "#!/bin/sh\necho ran >> '" + QFile::encodeName(marks) + "/" + name.toUtf8() + "'\ncat\n");
            QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
            return path;
        };
        QVERIFY(!gitIn(repo, {"config", "core.fsmonitor", program("fsmonitor")}).isNull());
        QVERIFY(!gitIn(repo, {"config", "filter.evil.clean", program("clean")}).isNull());
        QVERIFY(!gitIn(repo, {"config", "diff.evil.textconv", program("textconv")}).isNull());
        QVERIFY(write(repo + "/.gitattributes", "*.txt filter=evil diff=evil\n"));
        QVERIFY(write(repo + "/a.txt", "one\nchanged\nthree\n"));
        QDir(marks).removeRecursively();
        QVERIFY(QDir().mkpath(marks));

        qputenv("GIT_DIR", QFile::encodeName(canonical(dir.path()) + "/nowhere.git"));
        qputenv("GIT_INDEX_FILE", QFile::encodeName(canonical(dir.path()) + "/nowhere.index"));
        const auto none = [&](const char* after) {
            const QStringList ran = QDir(marks).entryList(QDir::Files);
            if (!ran.isEmpty()) qWarning() << after << "ran" << ran;
            return ran.isEmpty();
        };
        const Repository r = read(repo);
        QVERIFY(r.valid);
        QVERIFY(entry(r, "a.txt") != nullptr);
        QVERIFY(none("read"));
        QVERIFY(diff(repo).contains("+changed"));
        QVERIFY(diff(repo, repo + "/a.txt", DiffOf::Unstaged).contains("+changed"));
        QVERIFY(none("diff"));
        QVERIFY(!log(repo).isEmpty());
        QVERIFY(show(repo, "HEAD").contains("first"));
        QVERIFY(none("log, show"));
        QVERIFY(blame(repo, repo + "/a.txt").size() == 3);
        QVERIFY(none("blame"));
        QVERIFY(Tracker::instance()->readNow(repo) != nullptr);
        QJsonObject state = json(call("git_status", {{"path", repo}}));
        QCOMPARE(pathsIn(state.value("not staged").toArray()), QStringList({"a.txt"}));
        QVERIFY(text(call("git_diff", {{"path", repo}})).contains("+changed"));
        QVERIFY(none("the tools"));
        qunsetenv("GIT_DIR");
        qunsetenv("GIT_INDEX_FILE");
        QVERIFY(!QFileInfo::exists(canonical(dir.path()) + "/nowhere.index"));
    }

    // The tracker: read aside (null at first, then changed), read anew
    // when its index changes.
    void theTrackerFollowsTheRepository()
    {
        const QString repo = makeRepo("tracked");
        QVERIFY(!repo.isEmpty());
        Tracker* tracker = Tracker::instance();
        QSignalSpy changed(tracker, &Tracker::changed);
        const Repository* first = tracker->repositoryOf(repo + "/a.txt");
        if (first == nullptr) QTRY_VERIFY_WITH_TIMEOUT(tracker->repositoryOf(repo + "/a.txt") != nullptr, 10000);
        QVERIFY(tracker->entryOf(repo + "/a.txt") == nullptr);   // as committed
        QVERIFY(tracker->repositoryOf(canonical(dir.path()) + "/plain") == nullptr);
        QVERIFY(write(repo + "/a.txt", "changed\n"));
        tracker->refresh(repo);
        QTRY_VERIFY_WITH_TIMEOUT(tracker->entryOf(repo + "/a.txt") != nullptr, 10000);
        QCOMPARE(tracker->entryOf(repo + "/a.txt")->letter(), QStringLiteral("M"));
        // Staged by git itself: the index changes, the tracker follows.
        QVERIFY(!gitIn(repo, {"add", "a.txt"}).isNull());
        QTRY_VERIFY_WITH_TIMEOUT(tracker->entryOf(repo + "/a.txt") != nullptr && tracker->entryOf(repo + "/a.txt")->hasStaged(), 15000);
        QVERIFY(!changed.isEmpty());
        const Repository* now = tracker->readNow(repo);
        QVERIFY(now != nullptr && now->stagedCount() == 1);

        // Read again as it was: no change said (nothing drawn again).
        QTRY_VERIFY_WITH_TIMEOUT(!tracker->isReading(), 10000);
        changed.clear();
        tracker->refresh(repo);
        QTest::qWait(400);
        QTRY_VERIFY_WITH_TIMEOUT(!tracker->isReading(), 10000);
        QVERIFY(changed.isEmpty());
        // fresh(): the last read while it is recent (a menu opening runs no
        // git), else read now.
        QVERIFY(write(repo + "/b.txt", "changed\n"));
        QVERIFY(tracker->fresh(repo, 60000)->entryOf("b.txt") == nullptr);
        QVERIFY(tracker->fresh(repo, 0)->entryOf("b.txt") != nullptr);
        QCOMPARE(changed.count(), 1);
    }

    // The File Browser: each file's colour and letter, the branch beside
    // the repository's folder, the status line; its Git menu by what is
    // chosen; Show Git Status off.
    void theFileBrowserShowsGit()
    {
        const QString repo = makeRepo("browsed");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/a.txt", "one\nTWO\nthree\n"));
        QVERIFY(write(repo + "/new.txt", "new\n"));
        QVERIFY(QDir().mkpath(repo + "/sub"));
        QVERIFY(write(repo + "/sub/inner.txt", "inner\n"));   // untracked whole
        QVERIFY(write(repo + "/kept/k.txt", "k\n"));
        QVERIFY(commit(repo, "kept", false, {repo + "/kept/k.txt"}).ok());
        QVERIFY(write(repo + "/kept/k.txt", "K\n"));          // changes in it
        FileBrowser* fb = app->fileBrowserPanel();
        QVERIFY(fb != nullptr);
        fb->setView(FileBrowser::View::List);
        fb->setGitShown(true);
        // Its folder, from the folder above: the branch beside it.
        fb->setLocation(canonical(dir.path()));
        QTRY_VERIFY_WITH_TIMEOUT(fb->shownNames().contains("browsed"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(fb->indexOf(repo).data(GitBranchRole).toString(), QStringLiteral("main"), 10000);
        QVERIFY(fb->indexOf(repo).data(Qt::ToolTipRole).toString().contains("A git repository: main"));

        fb->setLocation(repo);
        QTRY_VERIFY_WITH_TIMEOUT(fb->shownNames().contains("new.txt") && fb->shownNames().contains("sub"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(fb->indexOf(repo + "/a.txt").data(GitLetterRole).toString(), QStringLiteral("M"), 10000);
        QCOMPARE(fb->indexOf(repo + "/new.txt").data(GitLetterRole).toString(), QStringLiteral("U"));
        QVERIFY(fb->indexOf(repo + "/b.txt").data(GitLetterRole).toString().isEmpty());
        QCOMPARE(fb->indexOf(repo + "/sub").data(GitLetterRole).toString(), QStringLiteral("U"));
        QCOMPARE(fb->indexOf(repo + "/kept").data(GitLetterRole).toString(), QStringLiteral("•"));   // changes in it
        QVERIFY(fb->indexOf(repo + "/kept").data(Qt::ToolTipRole).toString().contains("git: changes in it"));
        QVERIFY(fb->indexOf(repo + "/a.txt").data(Qt::ForegroundRole).value<QColor>().isValid());
        QVERIFY(!fb->indexOf(repo + "/b.txt").data(Qt::ForegroundRole).value<QColor>().isValid());
        QVERIFY(fb->indexOf(repo + "/a.txt").data(Qt::ToolTipRole).toString().contains("git: Modified, not staged"));
        QTRY_VERIFY_WITH_TIMEOUT(fb->statusLabel()->text().contains("⎇ main"), 5000);
        QVERIFY2(fb->statusLabel()->text().contains("4 changes"), qPrintable(fb->statusLabel()->text()));

        // The Git menu of a.txt: stage it.
        fb->selectPath(repo + "/a.txt");
        QMenu* menu = fb->contextMenuFor(repo + "/a.txt");
        QMenu* git = nullptr;
        for (QAction* a : menu->actions())
            if (a->menu() != nullptr && a->menu()->objectName() == "fbGitMenu") git = a->menu();
        QVERIFY(git != nullptr);
        QVERIFY(actionNamed(git, "gitStage") != nullptr && actionNamed(git, "gitStage")->isEnabled());
        QVERIFY(!actionNamed(git, "gitUnstage")->isEnabled());
        QVERIFY(actionNamed(git, "gitDiscard")->isEnabled());
        QVERIFY(actionNamed(git, "gitShowBlame")->isEnabled());
        QVERIFY(actionNamed(git, "gitShowHistory")->isEnabled());
        QVERIFY(actionNamed(git, "gitInitHere") == nullptr);
        actionNamed(git, "gitStage")->trigger();   // (filled anew as it opens: its actions too)
        QVERIFY(Tracker::instance()->readNow(repo)->entryOf("a.txt")->hasStaged());
        QTRY_VERIFY_WITH_TIMEOUT(fb->indexOf(repo + "/a.txt").data(Qt::ToolTipRole).toString().contains("Modified, staged"), 10000);
        delete menu;
        // The untracked file's: no history, no blame.
        menu = fb->contextMenuFor(repo + "/new.txt");
        for (QAction* a : menu->actions())
            if (a->menu() != nullptr && a->menu()->objectName() == "fbGitMenu") git = a->menu();
        QVERIFY(!actionNamed(git, "gitShowHistory")->isEnabled());
        QVERIFY(!actionNamed(git, "gitShowBlame")->isEnabled());
        QVERIFY(actionNamed(git, "gitIgnore")->isEnabled());
        delete menu;
        // The folder shown, in no repository: make one, clone one.
        const QString plain = canonical(dir.path()) + "/plain";
        QVERIFY(QDir().mkpath(plain));
        fb->setLocation(plain);
        menu = fb->contextMenuFor(QString());
        for (QAction* a : menu->actions())
            if (a->menu() != nullptr && a->menu()->objectName() == "fbGitMenu") git = a->menu();
        QVERIFY(actionNamed(git, "gitInitHere") != nullptr);
        QVERIFY(actionNamed(git, "gitCloneHere") != nullptr);
        QVERIFY(actionNamed(git, "gitCommit") == nullptr);
        delete menu;

        // Off: no colours, no letters, no branch.
        fb->setLocation(repo);
        QTRY_VERIFY_WITH_TIMEOUT(fb->shownNames().contains("new.txt"), 10000);
        fb->setGitShown(false);
        QVERIFY(fb->indexOf(repo + "/new.txt").data(GitLetterRole).toString().isEmpty());
        QVERIFY(!fb->indexOf(repo + "/new.txt").data(Qt::ForegroundRole).value<QColor>().isValid());
        QTRY_VERIFY_WITH_TIMEOUT(!fb->statusLabel()->text().contains("⎇"), 5000);
        fb->setGitShown(true);
    }

    // The Git menu, left of Help: what it can do with the document in
    // front and its repository; outside one, Create Repository alone.
    void theGitMenu()
    {
        QMenu* git = app->gitMenuWidget();
        QVERIFY(git != nullptr);
        const QList<QAction*> bar = app->menuBar()->actions();
        const qsizetype at = bar.indexOf(git->menuAction());
        QVERIFY(at >= 0);
        QAction* help = nullptr;
        for (qsizetype i = at + 1; i < bar.size() && help == nullptr; ++i)
            if (!bar.at(i)->isSeparator()) help = bar.at(i);
        QVERIFY(help != nullptr);
        QCOMPARE(help->text().remove('&'), QStringLiteral("Help"));

        const QString repo = makeRepo("menu");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/a.txt", "one\nTWO\nthree\n"));
        QVERIFY(app->gotoPage(repo + "/a.txt"));
        QCOMPARE(QFileInfo(app->gitFile()).canonicalFilePath(), repo + "/a.txt");
        QCOMPARE(canonical(app->gitRoot()), repo);
        emit git->aboutToShow();
        const auto on = [git](const char* name) {
            QAction* a = git->findChild<QAction*>(QLatin1String(name));
            return a != nullptr && a->isEnabled();
        };
        for (const char* name : {"gitCommit", "gitShowAllChanges", "gitFileChanges", "gitStageFile", "gitDiscardFile", "gitFileHistory",
                                 "gitBlame", "gitStageAll", "gitDiscardAll", "gitHistory", "gitFetch", "gitPull", "gitPush",
                                 "gitNewBranch", "gitStash", "gitNewTag", "gitIgnoreFile", "gitRemotes", "gitRefresh"})
            QVERIFY2(on(name), name);
        for (const char* name : {"gitUnstageFile", "gitUnstageAll", "gitAbort", "gitInit"}) QVERIFY2(!on(name), name);
        QVERIFY(submenuNamed(git, "gitSwitchMenu") != nullptr && submenuNamed(git, "gitSwitchMenu")->menuAction()->isEnabled());

        // Stage This File, from the menu.
        git->findChild<QAction*>("gitStageFile")->trigger();
        QVERIFY(Tracker::instance()->readNow(repo)->entryOf("a.txt")->hasStaged());
        emit git->aboutToShow();
        QVERIFY(on("gitUnstageFile") && on("gitUnstageAll") && !on("gitStageFile"));

        // New Branch..., its name asked.
        answerInput("from-the-menu");
        git->findChild<QAction*>("gitNewBranch")->trigger();
        QCOMPARE(read(repo).branch, QStringLiteral("from-the-menu"));
        QMenu* branches = submenuNamed(git, "gitSwitchMenu");
        emit branches->aboutToShow();
        QStringList names;
        for (QAction* a : branches->actions()) names << a->data().toString();
        QVERIFY2(names.contains("main") && names.contains("from-the-menu"), qPrintable(names.join(',')));
        for (QAction* a : branches->actions())
            if (a->data().toString() == "main") a->trigger();
        QCOMPARE(read(repo).branch, QStringLiteral("main"));

        // Discard This File: asked first - Cancel keeps it.
        answerBox("gitDiscardQuestion", "Cancel");
        git->findChild<QAction*>("gitDiscardFile")->trigger();
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\nTWO\nthree\n"));
        answerBox("gitDiscardQuestion", "Discard");
        git->findChild<QAction*>("gitDiscardFile")->trigger();
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\ntwo\nthree\n"));
        // (The document open of it is loaded again.)
        QTRY_VERIFY_WITH_TIMEOUT(read(repo).changedCount() == 0, 5000);

        // Outside a repository: Create Repository, and nothing else.
        for (QucsDoc* doc : app->allDocuments()) doc->setDocChanged(false);
        app->closeAllFiles();
        const QString plain = canonical(dir.path()) + "/menu-plain";
        QVERIFY(QDir().mkpath(plain));
        app->fileBrowserPanel()->setLocation(plain);
        emit git->aboutToShow();
        QVERIFY(on("gitInit") && on("gitClone") && on("gitRefresh"));
        for (const char* name : {"gitCommit", "gitHistory", "gitFetch", "gitPush", "gitStageAll", "gitFileChanges"}) QVERIFY2(!on(name), name);
        QVERIFY(!submenuNamed(git, "gitSwitchMenu")->menuAction()->isEnabled());
        answerBox("", "Yes");   // (asked)
        git->findChild<QAction*>("gitInit")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(plain + "/.git"), 5000);
        emit git->aboutToShow();
        QVERIFY(!on("gitInit") && on("gitCommit"));
    }

    // The status bar's chip: the branch and the document's letter; a
    // conflict in red; its menu - the file's state, what to do with it.
    void theStatusChip()
    {
        const QString repo = makeRepo("chip");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/a.txt", "one\nTWO\nthree\n"));
        auto* chip = app->findChild<QToolButton*>("statusGit");
        QVERIFY(chip != nullptr);
        QVERIFY(app->gotoPage(repo + "/a.txt"));
        Tracker::instance()->refresh(repo);
        QTRY_VERIFY_WITH_TIMEOUT(chip->toolTip().contains(QDir::toNativeSeparators("/chip\n"))
                                     && chip->text().contains("⎇ main") && chip->text().contains("M"),
                                 10000);
        QVERIFY2(chip->toolTip().contains("Branch: main"), qPrintable(chip->toolTip()));
        QVERIFY2(chip->toolTip().contains("This file: Modified, not staged"), qPrintable(chip->toolTip()));
        QVERIFY2(chip->toolTip().contains("Changes not committed: 1 (0 staged)"), qPrintable(chip->toolTip()));
        QCOMPARE(chip->property("tone").toString(), QString());

        chip->click();
        QMenu* menu = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((menu = app->findChild<QMenu*>("statusGitMenu")) != nullptr, 5000);
        QStringList texts;
        for (QAction* a : menu->actions()) texts << a->text();
        QVERIFY2(texts.join('|').contains("chip — main") && texts.join('|').contains("a.txt: Modified, not staged"), qPrintable(texts.join('|')));
        QVERIFY(actionNamed(menu, "gitStage")->isEnabled());
        QVERIFY(!actionNamed(menu, "gitUnstage")->isEnabled());
        QVERIFY(actionNamed(menu, "gitShowChanges")->isEnabled());
        QVERIFY(actionNamed(menu, "gitAbort") == nullptr);
        menu->close();

        // A merge in conflict: the chip says so, in red; its menu gives it up.
        QVERIFY(commit(repo, "TWO", false, {repo + "/a.txt"}).ok());
        QVERIFY(createBranch(repo, "other", "HEAD~1").ok());
        QVERIFY(write(repo + "/a.txt", "one\nother\nthree\n"));
        QVERIFY(commit(repo, "other", false, {repo + "/a.txt"}).ok());
        QVERIFY(switchTo(repo, "main").ok());
        QVERIFY(!merge(repo, "other").ok());
        app->reloadChangedFiles({repo + "/a.txt"});
        Tracker::instance()->refresh(repo);
        QTRY_VERIFY_WITH_TIMEOUT(chip->text().contains("merge under way"), 10000);
        QVERIFY(chip->text().contains("!"));
        QCOMPARE(chip->property("tone").toString(), QStringLiteral("error"));
        chip->click();
        QTRY_VERIFY_WITH_TIMEOUT((menu = app->findChild<QMenu*>("statusGitMenu")) != nullptr && menu->isVisible(), 5000);
        QAction* abortIt = actionNamed(menu, "gitAbort");
        QVERIFY(abortIt != nullptr);
        menu->close();
        answerBox("", "Yes");   // (QMessageBox::question: no name)
        abortIt->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(read(repo).operation.isEmpty(), 5000);
        Tracker::instance()->refresh(repo);
        QTRY_VERIFY_WITH_TIMEOUT(!chip->text().contains("under way"), 10000);

        // A document in no repository: no chip.
        for (QucsDoc* doc : app->allDocuments()) doc->setDocChanged(false);
        app->closeAllFiles();
        QVERIFY(write(canonical(dir.path()) + "/plain/loose.txt", "loose\n"));
        QVERIFY(app->gotoPage(canonical(dir.path()) + "/plain/loose.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!chip->isVisible(), 5000);
        app->closeAllFiles();
    }

    // The commit window: the lists, a file's changes, staged and
    // unstaged by its buttons; committed; nothing staged - asked to stage
    // everything.
    void theCommitWindow()
    {
        const QString repo = makeRepo("commit");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/a.txt", "one\nTWO\nthree\n"));
        QVERIFY(write(repo + "/new.txt", "new\n"));
        Commands::instance()->commit(repo);
        CommitDialog* d = nullptr;
        for (const QPointer<QDialog>& w : Commands::instance()->openWindows())
            if (auto* c = qobject_cast<CommitDialog*>(w.data()); c != nullptr && c->root() == repo) d = c;
        QVERIFY(d != nullptr);
        QVERIFY(d->headerLabel()->text().contains("on main"));
        QCOMPARE(d->unstagedList()->count(), 2);
        QCOMPARE(d->stagedList()->count(), 0);
        QVERIFY(!d->commitButton()->isEnabled());   // no message
        d->unstagedList()->setCurrentRow(0);
        QCOMPARE(d->unstagedList()->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("a.txt"));
        emit d->unstagedList()->itemClicked(d->unstagedList()->currentItem());
        QVERIFY(d->diffView()->toPlainText().contains("+TWO"));
        d->stageSelected();
        QCOMPARE(d->stagedList()->count(), 1);
        QCOMPARE(d->unstagedList()->count(), 1);
        d->stagedList()->setCurrentRow(0);
        d->unstageSelected();
        QCOMPARE(d->stagedList()->count(), 0);
        d->stageAll();
        QCOMPARE(d->stagedList()->count(), 2);
        d->unstageAll();
        QCOMPARE(d->stagedList()->count(), 0);
        d->unstagedList()->setCurrentRow(0);
        d->stageSelected();

        QSignalSpy committed(d, &CommitDialog::committed);
        d->messageEdit()->setPlainText("from the window");
        QVERIFY(d->commitButton()->isEnabled());
        d->commitNow(false);
        QVERIFY(committed.wait(30000));
        QCOMPARE(committed.first().at(1).toBool(), false);
        QCOMPARE(log(repo).first().subject, QStringLiteral("from the window"));
        QCOMPARE(d->stagedList()->count(), 0);
        QCOMPARE(d->unstagedList()->count(), 1);   // new.txt, left
        QVERIFY(d->messageEdit()->toPlainText().isEmpty());

        // Nothing staged: asked, and everything staged and committed.
        d->messageEdit()->setPlainText("all of it");
        answerBox("gitStageAllQuestion", "Stage All and Commit");
        d->commitNow(false);
        QVERIFY(committed.wait(30000));
        QCOMPARE(log(repo).first().subject, QStringLiteral("all of it"));
        QCOMPARE(read(repo).changedCount(), 0);
        // Amend: the last commit's message changed.
        d->amendBox()->setChecked(true);
        d->messageEdit()->setPlainText("all of it, amended");
        QVERIFY(d->commitButton()->isEnabled());
        d->commitNow(false);
        QVERIFY(committed.wait(30000));
        QCOMPARE(log(repo).first().subject, QStringLiteral("all of it, amended"));
        QCOMPARE(log(repo).size(), 3);
        d->close();
    }

    // The history window (a commit's menu) and the blame window.
    void theHistoryAndBlameWindows()
    {
        const QString repo = makeRepo("windows");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/a.txt", "one\ntwo\nthree\nfour\n"));
        QVERIFY(commit(repo, "four", false, {repo + "/a.txt"}).ok());
        HistoryDialog history(repo);
        QCOMPARE(history.commits()->topLevelItemCount(), 2);
        QVERIFY(history.commits()->topLevelItem(0)->text(0).endsWith("four"));
        QVERIFY(history.commits()->topLevelItem(0)->text(0).contains("HEAD -> main"));   // its refs
        history.commits()->setCurrentItem(history.commits()->topLevelItem(0));
        QTRY_VERIFY_WITH_TIMEOUT(history.details()->toPlainText().contains("+four"), 5000);
        QMenu* menu = history.menuFor(1);
        QVERIFY(menu != nullptr);
        for (const char* name : {"gitCopyHash", "gitCheckOutCommit", "gitBranchHere", "gitTagHere", "gitRevertCommit", "gitCherryPick"})
            QVERIFY2(menu->findChild<QAction*>(QLatin1String(name)) != nullptr, name);
        QVERIFY(submenuNamed(menu, "gitResetMenu") != nullptr);
        menu->findChild<QAction*>("gitCopyHash")->trigger();
        QCOMPARE(QApplication::clipboard()->text(), log(repo).at(1).hash);
        delete menu;
        menu = history.menuFor(0);   // "four" reverted
        menu->findChild<QAction*>("gitRevertCommit")->trigger();
        QCOMPARE(log(repo).first().subject.left(6), QStringLiteral("Revert"));
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\ntwo\nthree\n"));
        QCOMPARE(history.commits()->topLevelItemCount(), 3);   // read again
        delete menu;
        QVERIFY(!gitIn(repo, {"reset", "-q", "--hard", "HEAD~1"}).isNull());

        HistoryDialog fileHistory(repo, repo + "/b.txt");
        QCOMPARE(fileHistory.commits()->topLevelItemCount(), 1);

        BlameDialog blamed(repo, repo + "/a.txt");
        QCOMPARE(blamed.lines()->topLevelItemCount(), 4);
        QCOMPARE(blamed.lines()->topLevelItem(3)->text(4), QStringLiteral("four"));
        QCOMPARE(blamed.lines()->topLevelItem(0)->text(2), QStringLiteral("Tester"));
    }

    // A merge in conflict over a schematic open in a tab: git's file has
    // its markers, which no schematic reads - the tab keeps the version it
    // had, whole and saved as it was, asking nothing; a Save says what it
    // would write over; Claude's save is refused; once resolved, the tab
    // reads the file again. (Reloading it broke the tab: half its parts, no
    // wires or diagrams, clean - a Save would have written that over the
    // merge; and a question twice.)
    void aMergeConflictKeepsTheOpenSchematic()
    {
        const QString repo = makeRepo("conflict");
        QVERIFY(!repo.isEmpty());
        const QString example = QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/General Electronics/RC_filter_FFT.sch";
        QByteArray source = readAll(example);
        QVERIFY(source.contains("\"10nF\""));
        QVERIFY(write(repo + "/RC_filter_FFT.sch", source));
        QVERIFY(commit(repo, "filter", false, {repo + "/RC_filter_FFT.sch"}).ok());
        QVERIFY(createBranch(repo, "feature").ok());
        QVERIFY(write(repo + "/RC_filter_FFT.sch", QByteArray(source).replace("\"10nF\"", "\"22nF\"")));
        QVERIFY(commit(repo, "22nF", false, {repo + "/RC_filter_FFT.sch"}).ok());
        QVERIFY(switchTo(repo, "main").ok());
        QVERIFY(write(repo + "/RC_filter_FFT.sch", QByteArray(source).replace("\"10nF\"", "\"33nF\"")));
        QVERIFY(commit(repo, "33nF", false, {repo + "/RC_filter_FFT.sch"}).ok());

        QVERIFY(app->gotoPage(repo + "/RC_filter_FFT.sch"));
        auto* sch = dynamic_cast<Schematic*>(app->getDoc());
        QVERIFY(sch != nullptr);
        const auto counts = [sch] {
            return QList<qsizetype>{qsizetype(sch->a_DocComps.size()), qsizetype(sch->a_DocWires.size()), qsizetype(sch->a_DocDiags.size())};
        };
        const QList<qsizetype> whole = counts();
        QVERIFY(whole.at(0) == 8 && whole.at(1) > 4 && whole.at(2) == 2);
        QVERIFY(sch->documentText().contains("\"33nF\""));

        BoxCounter boxes(QStringLiteral("No"), {QStringLiteral("gitResolveQuestion")});
        QVERIFY(!merge(repo, "feature").ok());
        QVERIFY(readAll(repo + "/RC_filter_FFT.sch").contains("<<<<<<< HEAD"));
        QTest::qWait(3500);   // (the window's watch: its timers)
        QVERIFY2(boxes.texts().isEmpty(), qPrintable(boxes.texts().join(" | ")));
        QCOMPARE(dynamic_cast<Schematic*>(app->getDoc()), sch);
        QCOMPARE(counts(), whole);
        QVERIFY(sch->documentText().contains("\"33nF\""));
        QVERIFY(!sch->getDocChanged());
        QVERIFY(app->keptInConflict(repo + "/RC_filter_FFT.sch"));
        QVERIFY(app->statusBar()->currentMessage().contains("conflict"));
        QVERIFY(readAll(repo + "/RC_filter_FFT.sch").contains("<<<<<<< HEAD"));   // (nothing written)
        // Said once: a watch's next look at the same file asks nothing.
        QTest::qWait(2000);
        QVERIFY(boxes.texts().isEmpty());
        QCOMPARE(counts(), whole);

        // The chip says so; its menu resolves it.
        auto* chip = app->findChild<QToolButton*>("statusGit");
        Tracker::instance()->refresh(repo);
        QTRY_VERIFY_WITH_TIMEOUT(chip->toolTip().contains("version from before the merge"), 10000);
        QCOMPARE(chip->property("tone").toString(), QStringLiteral("error"));
        QMenu menu;
        Commands::instance()->fillFileMenu(&menu, repo + "/RC_filter_FFT.sch");
        QMenu* resolve = submenuNamed(&menu, "gitResolveMenu");
        QVERIFY(resolve != nullptr);
        for (const char* name : {"gitKeepMine", "gitTakeTheirs", "gitOpenBoth"})
            QVERIFY2(resolve->findChild<QAction*>(QLatin1String(name)) != nullptr, name);
        QVERIFY(!resolve->findChild<QAction*>("gitMarkResolved")->isEnabled());   // (marks in it)
        // The Git menu's too, for the document in front.
        emit app->gitMenuWidget()->aboutToShow();
        QVERIFY(app->gitMenuWidget()->findChild<QMenu*>("gitResolveFileMenu")->menuAction()->isEnabled());

        // A Save asks, saying what it writes over; no (Cancel): nothing written.
        QVERIFY(!app->saveFile(sch));
        const QStringList asked = boxes.take();
        QCOMPARE(asked.size(), 1);
        QVERIFY2(asked.first().contains("in conflict"), qPrintable(asked.first()));
        // Asked though the file's date says nothing (one kept from before).
        {
            QFile f(repo + "/RC_filter_FFT.sch");
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(sch->getLastSaved().addSecs(-60), QFileDevice::FileModificationTime));
        }
        QVERIFY(!app->saveFile(sch));
        QCOMPARE(boxes.take().size(), 1);
        QVERIFY(readAll(repo + "/RC_filter_FFT.sch").contains("<<<<<<< HEAD"));
        QVERIFY(readAll(repo + "/RC_filter_FFT.sch").contains("<<<<<<< HEAD"));
        // Claude's save: refused, unless 'replace'.
        QJsonObject r = call("save_document", {{"path", repo + "/RC_filter_FFT.sch"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("in conflict") && text(r).contains("replace"), qPrintable(text(r)));
        QVERIFY(control->irreversible("save_document", {{"replace", true}}));
        QVERIFY(!control->irreversible("save_document", {}));
        // git_status names the tab kept.
        const QJsonObject state = json(call("git_status", {{"path", repo}}));
        QCOMPARE(state.value("conflicts").toArray(), QJsonArray({"RC_filter_FFT.sch"}));
        QCOMPARE(state.value("kept in their tabs").toObject().value("files").toArray().size(), 1);

        // Take Theirs, from the menu (asked): the file theirs, the tab read again.
        answerBox("gitResolveQuestion", "Take Theirs");
        resolve->findChild<QAction*>("gitTakeTheirs")->trigger();
        QVERIFY(!app->keptInConflict(repo + "/RC_filter_FFT.sch"));   // (its marks gone, before the tab is read again)
        QVERIFY(!readAll(repo + "/RC_filter_FFT.sch").contains("<<<<<<<"));
        QVERIFY(readAll(repo + "/RC_filter_FFT.sch").contains("\"22nF\""));
        QVERIFY(read(repo).conflictCount() == 0);
        QTRY_VERIFY_WITH_TIMEOUT(sch->documentText().contains("\"22nF\""), 10000);
        QCOMPARE(counts(), whole);
        QVERIFY(!app->keptInConflict(repo + "/RC_filter_FFT.sch"));
        QVERIFY(commit(repo, "merged").ok());
        // Theirs whole, the merge's tree is theirs: still in the history
        // of the repository's folder.
        QCOMPARE(log(repo, repo, 1).first().subject, QStringLiteral("merged"));
        QCOMPARE(log(repo, repo, 1).first().parents.size(), 2);
        QVERIFY(boxes.texts().isEmpty());
        sch->setDocChanged(false);
        app->closeAllFiles();
    }

    // Claude meets the conflict: the merge's answer names the tab kept;
    // git_resolve shows the sides (a schematic's, part by part), opens both
    // versions beside it, takes one; a revert in conflict the same.
    void claudeResolvesAConflict()
    {
        const QString repo = makeRepo("claude-conflict");
        QVERIFY(!repo.isEmpty());
        const QString example = QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/General Electronics/RC_filter_FFT.sch";
        const QByteArray source = readAll(example);
        const QString file = repo + "/RC_filter_FFT.sch";
        QVERIFY(write(file, source));
        QVERIFY(commit(repo, "filter", false, {file}).ok());
        QVERIFY(createBranch(repo, "feature").ok());
        QVERIFY(write(file, QByteArray(source).replace("\"10nF\"", "\"22nF\"")));
        QVERIFY(commit(repo, "22nF", false, {file}).ok());
        QVERIFY(switchTo(repo, "main").ok());
        QVERIFY(write(file, QByteArray(source).replace("\"10nF\"", "\"33nF\"").replace("\"100k\"", "\"47k\"")));
        QVERIFY(commit(repo, "33nF", false, {file}).ok());
        QVERIFY(app->gotoPage(file));
        auto* sch = dynamic_cast<Schematic*>(app->getDoc());
        QVERIFY(sch != nullptr);
        BoxCounter boxes;

        QJsonObject r = call("git_branch", {{"path", repo}, {"action", "merge"}, {"name", "feature"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("conflicts").toArray(), QJsonArray({"RC_filter_FFT.sch"}));
        QVERIFY(o.value("merge").toString().contains("git_resolve"));
        QCOMPARE(o.value("kept in their tabs").toObject().value("files").toArray().size(), 1);
        QVERIFY(sch->documentText().contains("\"33nF\""));
        QCOMPARE(qsizetype(sch->a_DocComps.size()), qsizetype(8));

        // show: the sides, and what each changed by part.
        r = call("git_resolve", {{"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        o = json(r);
        QVERIFY(o.value("mine").toString().contains("\"33nF\""));
        QVERIFY(o.value("theirs").toString().contains("\"22nF\""));
        QVERIFY(o.value("base").toString().contains("\"10nF\""));
        QVERIFY(o.value("marks in the file").toBool());
        const QString changes = o.value("changes").toString();
        QVERIFY2(changes.contains("C1 (C): C \"10nF\" \u2192 \"33nF\"") && changes.contains("R1 (R): R \"100k\" \u2192 \"47k\"")
                     && changes.contains("C1 (C): C \"10nF\" \u2192 \"22nF\""),
                 qPrintable(changes));
        QVERIFY(o.value("tab").toString().contains("mine"));
        QVERIFY(!control->irreversible("git_resolve", {{"path", file}}));
        QVERIFY(control->irreversible("git_resolve", {{"path", file}, {"action", "take_ours"}}));
        QVERIFY(failed(call("git_resolve", {{"path", repo + "/a.txt"}})));   // not in conflict
        QVERIFY(failed(call("git_resolve", {{"path", file}, {"action", "merge"}})));

        // open: both versions beside it, opened as schematics (no question).
        r = call("git_resolve", {{"path", file}, {"action", "open"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray opened = json(r).value("opened").toArray();
        QCOMPARE(opened.size(), 2);
        QVERIFY(QFileInfo::exists(repo + "/RC_filter_FFT (mine).sch") && QFileInfo::exists(repo + "/RC_filter_FFT (theirs).sch"));
        QVERIFY(readAll(repo + "/RC_filter_FFT (theirs).sch").contains("\"22nF\""));
        QVERIFY(app->findDoc(repo + "/RC_filter_FFT (theirs).sch") != nullptr);
        QVERIFY(boxes.texts().isEmpty());
        for (const QString& v : {QStringLiteral("mine"), QStringLiteral("theirs")}) {
            const QString copy = repo + "/RC_filter_FFT (" + v + ").sch";
            QVERIFY(!failed(call("close_document", {{"path", copy}, {"unsaved", "discard"}})));
            QVERIFY(QFile::remove(copy));
        }

        // take_ours: mine, staged; the tab as it was (its file the same now).
        r = call("git_resolve", {{"path", file}, {"action", "take_ours"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!json(r).contains("conflicts"));
        QVERIFY(readAll(file).contains("\"33nF\"") && !readAll(file).contains("<<<<<<<"));
        QVERIFY(sch->documentText().contains("\"33nF\""));
        QCOMPARE(qsizetype(sch->a_DocComps.size()), qsizetype(8));
        r = call("git_commit", {{"path", repo}, {"message", "merged, mine"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // The repository's history, given as its folder: the merge in it.
        QJsonObject top = json(call("git_log", {{"path", repo}, {"max", 1}})).value("commits").toArray().first().toObject();
        QCOMPARE(top.value("subject").toString(), QStringLiteral("merged, mine"));
        QVERIFY(top.value("merge").toBool());
        QVERIFY(boxes.texts().isEmpty());
        sch->setDocChanged(false);
        app->closeAllFiles();
    }

    // The other kinds of document open through a merge in conflict: a
    // symbol and a data display (schematics both) are kept as they were;
    // a text and a Python script are read again, the marks in sight to be
    // edited out - and the status bar says so.
    void otherDocumentsInAConflict()
    {
        const QString repo = makeRepo("kinds");
        QVERIFY(!repo.isEmpty());
        const QString version = QStringLiteral(PACKAGE_VERSION);
        const auto symbol = [&](const char* colour) {
            return QStringLiteral("<Qucs Schematic %1>\n<Symbol>\n  <Line -20 -10 40 0 %2 2 1>\n  <.PortSym -30 0 1 0 P1>\n</Symbol>\n")
                .arg(version, QLatin1String(colour)).toUtf8();
        };
        const auto display = [&](const char* words) {
            return QStringLiteral("<Qucs Schematic %1>\n<Properties>\n  <View=0,0,800,600,1,0,0>\n</Properties>\n<Symbol>\n"
                                  "</Symbol>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n"
                                  "  <Text 100 100 12 #000000 0 \"%2\">\n  <Text 100 200 12 #000000 0 \"kept\">\n</Paintings>\n")
                .arg(version, QLatin1String(words)).toUtf8();
        };
        QVERIFY(write(repo + "/part.sym", symbol("#000080")));
        QVERIFY(write(repo + "/plot.dpl", display("first")));
        QVERIFY(write(repo + "/notes.txt", "first\n"));
        QVERIFY(write(repo + "/run.py", "x = 1\n"));
        QVERIFY(stage(repo, {}).ok() && commit(repo, "kinds").ok());
        QVERIFY(createBranch(repo, "feature").ok());
        QVERIFY(write(repo + "/part.sym", symbol("#ff0000")));
        QVERIFY(write(repo + "/plot.dpl", display("theirs")));
        QVERIFY(write(repo + "/notes.txt", "theirs\n"));
        QVERIFY(write(repo + "/run.py", "x = 2\n"));
        QVERIFY(stage(repo, {}).ok() && commit(repo, "theirs").ok());
        QVERIFY(switchTo(repo, "main").ok());
        QVERIFY(write(repo + "/part.sym", symbol("#00ff00")));
        QVERIFY(write(repo + "/plot.dpl", display("mine")));
        QVERIFY(write(repo + "/notes.txt", "mine\n"));
        QVERIFY(write(repo + "/run.py", "x = 3\n"));
        QVERIFY(stage(repo, {}).ok() && commit(repo, "mine").ok());
        for (const char* f : {"part.sym", "plot.dpl", "notes.txt", "run.py"}) QVERIFY2(app->gotoPage(repo + "/" + f), f);
        auto* sym = dynamic_cast<Schematic*>(app->findDoc(repo + "/part.sym"));
        auto* dpl = dynamic_cast<Schematic*>(app->findDoc(repo + "/plot.dpl"));
        auto* txt = dynamic_cast<TextDoc*>(app->findDoc(repo + "/notes.txt"));
        auto* py = dynamic_cast<TextDoc*>(app->findDoc(repo + "/run.py"));
        QVERIFY(sym != nullptr && dpl != nullptr && txt != nullptr && py != nullptr);
        const QString symBefore = sym->documentText(), dplBefore = dpl->documentText();
        QVERIFY(symBefore.contains("#00ff00") && dplBefore.contains("\"mine\""));

        BoxCounter boxes;
        const QJsonObject r = call("git_branch", {{"path", repo}, {"action", "merge"}, {"name", "feature"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("conflicts").toArray().size(), 4);
        const QJsonArray kept = json(r).value("kept in their tabs").toObject().value("files").toArray();
        QCOMPARE(kept.size(), 2);
        QCOMPARE(json(r).value("loaded again").toArray().size(), 2);
        QCOMPARE(sym->documentText(), symBefore);
        QCOMPARE(dpl->documentText(), dplBefore);
        QVERIFY(app->keptInConflict(repo + "/part.sym") && app->keptInConflict(repo + "/plot.dpl"));
        QVERIFY(txt->toPlainText().contains("<<<<<<< HEAD") && txt->toPlainText().contains(">>>>>>> feature"));
        QVERIFY(py->toPlainText().contains("<<<<<<< HEAD"));
        QVERIFY(!app->keptInConflict(repo + "/notes.txt"));
        QTest::qWait(1500);   // (the window's watch: nothing more)
        QVERIFY2(boxes.texts().isEmpty(), qPrintable(boxes.texts().join(" | ")));
        QCOMPARE(sym->documentText(), symBefore);
        // The text resolved by hand, staged: resolved.
        txt->setPlainText("mine and theirs\n");
        QVERIFY(app->saveFile(txt));
        QVERIFY(!failed(call("git_stage", {{"path", repo}, {"paths", QJsonArray{"notes.txt"}}})));
        QVERIFY(!json(call("git_status", {{"path", repo}})).value("conflicts").toArray().contains("notes.txt"));
        QVERIFY(!failed(call("git_abort", {{"path", repo}})));
        QTRY_VERIFY_WITH_TIMEOUT(txt->toPlainText() == "mine\n" && sym->documentText().contains("#00ff00"), 10000);
        QVERIFY(boxes.texts().isEmpty());
        for (QucsDoc* d : app->allDocuments()) d->setDocChanged(false);
        app->closeAllFiles();
    }

    // A merge whose conflicts were resolved as the branch had them -
    // nothing staged: the Commit window still records it (it said there
    // was nothing to commit); a stash brought back in conflict is said,
    // not an error, and kept.
    void aMergeResolvedAsMineIsCommitted()
    {
        const QString repo = makeRepo("merge-mine");
        QVERIFY(!repo.isEmpty());
        QVERIFY(createBranch(repo, "feature").ok());
        QVERIFY(write(repo + "/a.txt", "one\nfeature\nthree\n"));
        QVERIFY(commit(repo, "feature", false, {repo + "/a.txt"}).ok());
        QVERIFY(switchTo(repo, "main").ok());
        QVERIFY(write(repo + "/a.txt", "one\nmain\nthree\n"));
        QVERIFY(commit(repo, "main", false, {repo + "/a.txt"}).ok());
        QVERIFY(!merge(repo, "feature").ok());
        QVERIFY(resolve(repo, repo + "/a.txt", "ours").ok());
        QCOMPARE(read(repo).stagedCount(), 0);
        QCOMPARE(read(repo).operation, QStringLiteral("merge"));
        Commands::instance()->commit(repo);
        CommitDialog* d = nullptr;
        for (const QPointer<QDialog>& w : Commands::instance()->openWindows())
            if (auto* c = qobject_cast<CommitDialog*>(w.data()); c != nullptr && c->root() == repo) d = c;
        QVERIFY(d != nullptr);
        QVERIFY(d->headerLabel()->text().contains("merge under way"));
        QSignalSpy committed(d, &CommitDialog::committed);
        d->messageEdit()->setPlainText("Merge feature, mine kept");
        QVERIFY(d->commitButton()->isEnabled());
        d->commitNow(false);
        QVERIFY(committed.wait(30000));
        QCOMPARE(log(repo).first().parents.size(), 2);
        QVERIFY(read(repo).operation.isEmpty());
        d->close();

        // A stash brought back over a change of its lines: in conflict, said.
        QVERIFY(write(repo + "/a.txt", "one\nstashed\nthree\n"));
        QVERIFY(stash(repo, "aside").ok());
        QVERIFY(write(repo + "/a.txt", "one\nin the way\nthree\n"));
        QVERIFY(commit(repo, "in the way", false, {repo + "/a.txt"}).ok());
        const QJsonObject r = call("git_stash", {{"path", repo}, {"action", "pop"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("stash").toString().contains("in conflict"));
        QCOMPARE(json(r).value("conflicts").toArray(), QJsonArray({"a.txt"}));
        QCOMPARE(stashes(repo).size(), 1);   // (kept)
    }

    // A file that does not read, loaded again (here: answered No to its
    // unknown part): the tab gets back what it held, asked once - not a
    // document half read that looks saved.
    void aFailedReloadPutsTheDocumentBack()
    {
        const QString dir2 = canonical(dir.path()) + "/unreadable";
        QVERIFY(QDir().mkpath(dir2));
        const QString file = dir2 + "/RC_filter_FFT.sch";
        const QByteArray source = readAll(QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/General Electronics/RC_filter_FFT.sch");
        QVERIFY(write(file, source));
        QVERIFY(app->gotoPage(file));
        auto* sch = dynamic_cast<Schematic*>(app->getDoc());
        QVERIFY(sch != nullptr);
        const QString before = sch->documentText();
        BoxCounter boxes;   // (No to "load anyway?")
        QTest::qWait(1100);   // (a newer date than its load)
        QVERIFY(replaceFile(file, QByteArray(source).replace("<Components>\n", "<Components>\n  <Bogus X1 1 0 0 0 0 0 0 \"1\" 1>\n")));
        QTRY_VERIFY_WITH_TIMEOUT(!boxes.texts().isEmpty(), 10000);
        QTest::qWait(3500);   // (and the watch's next look: set again, the file new)
        QCOMPARE(boxes.texts().size(), 1);
        // Told of again, the file as it was (another notice of one change,
        // Claude's edit of it): not read, nor asked about, again.
        app->reloadChangedFiles({file});
        QTest::qWait(300);
        QCOMPARE(boxes.texts().size(), 1);
        QVERIFY(boxes.texts().first().contains("Unknown component"));
        QCOMPARE(dynamic_cast<Schematic*>(app->getDoc()), sch);
        QCOMPARE(qsizetype(sch->a_DocComps.size()), qsizetype(8));
        QCOMPARE(sch->documentText(), before);
        QVERIFY(!sch->getDocChanged());
        QVERIFY(app->statusBar()->currentMessage().contains("could not be read"));
        // A Save asks before writing over the file (Cancel).
        boxes.take();
        QVERIFY(!app->saveFile(sch));
        QCOMPARE(boxes.take().size(), 1);
        // Readable again: read.
        QTest::qWait(1100);
        QVERIFY(write(file, QByteArray(source).replace("\"10nF\"", "\"47nF\"")));
        QTRY_VERIFY_WITH_TIMEOUT(sch->documentText().contains("\"47nF\""), 10000);
        QVERIFY(boxes.texts().isEmpty());
        // Its symbol being edited: a file that does not read leaves it so.
        app->slotSymbolEdit();
        QVERIFY(sch->getSymbolMode());
        QVERIFY(app->saveFile(sch));   // (its symbol, drawn as it opened, saved)
        QVERIFY(!sch->getDocChanged());
        QTest::qWait(1100);
        QVERIFY(replaceFile(file, QByteArray(source).replace("<Components>\n", "<Components>\n  <Bogus X2 1 0 0 0 0 0 0 \"1\" 1>\n")));
        QTRY_VERIFY_WITH_TIMEOUT(boxes.texts().size() == 1, 10000);
        QTest::qWait(500);
        QVERIFY(sch->getSymbolMode());
        QVERIFY(sch->documentText().contains("\"47nF\""));
        QCOMPARE(qsizetype(sch->a_DocComps.size()), qsizetype(8));
        app->slotSymbolEdit();
        QVERIFY(!sch->getSymbolMode());
        sch->setDocChanged(false);
        app->closeAllFiles();
    }

    // A schematic with git's conflict marks, opened: said, with Open Both
    // Versions and Open as Text - not read as one (unknown parts asked
    // about); Claude's open_document says the same and how to go on.
    void aConflictedSchematicIsNotOpenedAsOne()
    {
        const QString repo = makeRepo("open-conflict");
        QVERIFY(!repo.isEmpty());
        const QString file = repo + "/RC_filter_FFT.sch";
        const QByteArray source = readAll(QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/General Electronics/RC_filter_FFT.sch");
        QVERIFY(write(file, source));
        QVERIFY(commit(repo, "filter", false, {file}).ok());
        QVERIFY(createBranch(repo, "feature").ok());
        QVERIFY(write(file, QByteArray(source).replace("\"10nF\"", "\"22nF\"")));
        QVERIFY(commit(repo, "22nF", false, {file}).ok());
        QVERIFY(switchTo(repo, "main").ok());
        QVERIFY(write(file, QByteArray(source).replace("\"10nF\"", "\"33nF\"")));
        QVERIFY(commit(repo, "33nF", false, {file}).ok());
        QVERIFY(!merge(repo, "feature").ok());
        QVERIFY(hasConflictMarkers(file));
        QVERIFY(!hasConflictMarkers(repo + "/a.txt"));

        {
            BoxCounter open(QStringLiteral("Open Both Versions"));
            QVERIFY(!app->gotoPage(file));
            QCOMPARE(open.texts().size(), 1);
            QVERIFY(open.texts().first().contains("is in conflict"));
        }
        QVERIFY(app->findDoc(file) == nullptr);
        QVERIFY(app->findDoc(repo + "/RC_filter_FFT (mine).sch") != nullptr);
        QVERIFY(app->findDoc(repo + "/RC_filter_FFT (theirs).sch") != nullptr);
        for (QucsDoc* d : app->allDocuments()) d->setDocChanged(false);
        app->closeAllFiles();
        {
            BoxCounter asText(QStringLiteral("Open as Text"));
            QVERIFY(!app->gotoPage(file));
            QCOMPARE(asText.texts().size(), 1);
        }
        QTRY_VERIFY(app->getDoc() != nullptr && dynamic_cast<TextDoc*>(app->getDoc()) != nullptr);
        QVERIFY(dynamic_cast<TextDoc*>(app->getDoc())->toPlainText().contains("<<<<<<< HEAD"));
        for (QucsDoc* d : app->allDocuments()) d->setDocChanged(false);
        app->closeAllFiles();
        const QJsonObject r = call("open_document", {{"path", file}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("in conflict") && text(r).contains("git_resolve"), qPrintable(text(r)));
        // A side whole; the copies were made for looking.
        QVERIFY(resolve(repo, file, "theirs").ok());
        QVERIFY(!hasConflictMarkers(file));
        QVERIFY(readAll(file).contains("\"22nF\""));
        QVERIFY(!resolve(repo, file, "theirs").ok());   // (not in conflict now)
        QVERIFY(!resolve(repo, file, "mine").ok());
    }

    // A relative path, the same for every git tool: from the open project's
    // folder, the workspace, the document's folder - the first where it is,
    // or, not made yet, the first in a repository (git_ignore looked in the
    // project alone). A file in .gitignore staged: said which and why.
    void relativePathsAreFoundAlike()
    {
        const QString ws = QucsSettings.qucsWorkspaceDir.absolutePath();
        const QString repo = ws + "/_git_smoke";
        QVERIFY(QDir().mkpath(repo));
        QVERIFY(init(repo).ok());
        QVERIFY(write(repo + "/a.txt", "a\n"));
        const QString project = ws + "/gds_demo_prj";
        QVERIFY(QDir().mkpath(project));
        const QDir was = QucsSettings.QucsWorkDir;
        QucsSettings.QucsWorkDir.setPath(project);   // (a project open elsewhere)
        for (QucsDoc* d : app->allDocuments()) d->setDocChanged(false);
        app->closeAllFiles();
        const QString elsewhere = canonical(dir.path()) + "/browsed-elsewhere";
        QVERIFY(QDir().mkpath(elsewhere));
        app->fileBrowserPanel()->setLocation(elsewhere);   // (in no repository)
        QJsonObject r = call("git_ignore", {{"path", "_git_smoke/run.dat.ngspice"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(readAll(repo + "/.gitignore").contains("/run.dat.ngspice"));
        r = call("git_status", {{"path", "_git_smoke/a.txt"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("path").toObject().value("path").toString(), QStringLiteral("a.txt"));
        r = call("git_stage", {{"paths", QJsonArray{"_git_smoke/a.txt"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("staged").toArray().size(), 1);
        r = call("git_diff", {{"path", "_git_smoke"}, {"of", "staged"}});
        QVERIFY(text(r).contains("+a"));
        // In the project too: the project's first.
        QVERIFY(QDir().mkpath(project + "/_git_smoke"));
        QVERIFY(write(project + "/_git_smoke/a.txt", "p\n"));
        r = call("git_status", {{"path", "_git_smoke/a.txt"}});
        QVERIFY(failed(r));   // (the project's folder is in no repository)
        QVERIFY(QDir(project + "/_git_smoke").removeRecursively());

        // In .gitignore: said.
        QVERIFY(write(repo + "/run.dat.ngspice", "0\n"));
        r = call("git_stage", {{"paths", QJsonArray{"_git_smoke/run.dat.ngspice"}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("in .gitignore") && text(r).contains("run.dat.ngspice"), qPrintable(text(r)));
        QucsSettings.QucsWorkDir = was;
    }

    // A part changed - a property, a move, a turn, replaced, edited in
    // its dialog - stays where it was among the parts: the file keeps its
    // order (it went last: a diff of the whole line, two branches'
    // edits in conflict at the list's end).
    void partsKeepTheirPlace()
    {
        const QString file = canonical(dir.path()) + "/order/order.sch";
        QVERIFY(QDir().mkpath(QFileInfo(file).absolutePath()));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const auto& [type, x] : std::initializer_list<std::pair<const char*, int>>{{"R", 100}, {"C", 200}, {"L", 300}, {"R", 400}})
            QVERIFY(!failed(call("add_component", {{"type", type}, {"x", x}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", file}})));
        auto* sch = dynamic_cast<Schematic*>(app->getDoc());
        QVERIFY(sch != nullptr);
        const auto order = [sch] {
            QStringList names;
            for (const Component* c : sch->a_DocComps) names << c->Name;
            return names;
        };
        const QStringList start{"R1", "C1", "L1", "R2"};
        QCOMPARE(order(), start);
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "2k"}}}})));
        QCOMPARE(order(), start);
        QVERIFY(!failed(call("edit_component", {{"name", "C1"}, {"x", 220}, {"y", 160}})));
        QCOMPARE(order(), start);
        QVERIFY(!failed(call("edit_component", {{"name", "L1"}, {"rotation", 1}})));
        QCOMPARE(order(), start);
        QVERIFY(!failed(call("replace_component", {{"name", "C1"}, {"type", "C"}})));
        QCOMPARE(order().size(), 4);
        QCOMPARE(order().at(1).left(1), QStringLiteral("C"));
        // Its properties' dialog, OK'd.
        Component* r1 = sch->getComponentByName("R1");
        QVERIFY(r1 != nullptr);
        auto* timer = new QTimer(this);
        timer->setInterval(20);
        connect(timer, &QTimer::timeout, this, [timer] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* d = qobject_cast<QDialog*>(w); d != nullptr && d->isVisible() && QString(d->metaObject()->className()) == "ComponentDialog") {
                    timer->stop();
                    d->accept();
                }
        });
        timer->start();
        app->view->focusElement = r1;
        QMouseEvent click(QEvent::MouseButtonDblClick, QPointF(10, 10), QPointF(10, 10), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        app->view->editElement(sch, &click);
        timer->deleteLater();
        QCOMPARE(order().first(), QStringLiteral("R1"));
        // And as saved.
        QVERIFY(!failed(call("save_document", {})));
        QStringList saved;
        for (const QString& l : QString::fromUtf8(readAll(file)).split('\n'))
            if (l.startsWith("  <") && l.split(' ', Qt::SkipEmptyParts).size() > 8) saved << l.split(' ', Qt::SkipEmptyParts).at(1);
        QCOMPARE(saved.first(), QStringLiteral("R1"));
        QCOMPARE(saved.last(), QStringLiteral("R2"));
        sch->setDocChanged(false);
        app->closeAllFiles();
    }

    // The view (View=: scroll and zoom, which an opened schematic does not
    // use - it is fitted to the window) is written as the file had it: a
    // save after a change changes the change's lines alone.
    void theViewLineStaysAsTheFileHasIt()
    {
        const QString file = canonical(dir.path()) + "/view/RC_filter_FFT.sch";
        QVERIFY(write(file, readAll(QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/General Electronics/RC_filter_FFT.sch")));
        const QRegularExpression view(QStringLiteral("<View=[^>]*>"));
        const QString original = view.match(QString::fromUtf8(readAll(file))).captured(0);
        QVERIFY(!original.isEmpty());
        QVERIFY(app->gotoPage(file));
        auto* sch = dynamic_cast<Schematic*>(app->getDoc());
        QVERIFY(sch != nullptr);
        // Saved once by this version (the old file's header grows), the
        // view as it had it.
        sch->zoomBy(0.8);
        QVERIFY(!failed(call("save_document", {})));
        const QByteArray source = readAll(file);
        QCOMPARE(view.match(QString::fromUtf8(source)).captured(0), original);
        sch->zoomBy(1.7);
        QVERIFY(!failed(call("edit_component", {{"name", "C1"}, {"properties", QJsonObject{{"C", "33nF"}}}})));
        QVERIFY(!failed(call("save_document", {})));
        const QString saved = QString::fromUtf8(readAll(file));
        QCOMPARE(view.match(saved).captured(0), original);
        // The diff: the value's line alone.
        QStringList changed;
        const QStringList a = QString::fromUtf8(source).split('\n'), b = saved.split('\n');
        QCOMPARE(a.size(), b.size());
        for (qsizetype i = 0; i < a.size(); ++i)
            if (a.at(i) != b.at(i)) changed << b.at(i);
        QCOMPARE(changed.size(), 1);
        QVERIFY(changed.at(0).contains("\"33nF\""));
        sch->setDocChanged(false);
        app->closeAllFiles();
        // A new one: its view written once, then as it is.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}})));
        const QString fresh = canonical(dir.path()) + "/view/fresh.sch";
        QVERIFY(!failed(call("save_document", {{"as", fresh}})));
        const QString first = view.match(QString::fromUtf8(readAll(fresh))).captured(0);
        QVERIFY(!first.isEmpty());
        dynamic_cast<Schematic*>(app->getDoc())->zoomBy(0.5);
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"x", 300}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {})));
        QCOMPARE(view.match(QString::fromUtf8(readAll(fresh))).captured(0), first);
        app->getDoc()->setDocChanged(false);
        app->closeAllFiles();
    }

    // What changed in a schematic, part by part: the parts added, removed,
    // moved, turned, their properties by name; the wires and labels, the
    // diagrams, the settings - not the view. In git_diff, git_show, the
    // Show Changes, Commit and History windows.
    void schematicChangesPartByPart()
    {
        const QString base = QString::fromUtf8(readAll(QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/General Electronics/RC_filter_FFT.sch"));
        QString changed = base;
        changed.replace("\"10nF\"", "\"33nF\"");
        changed.replace(QRegularExpression("<View=[^>]*>"), "<View=-1,-2,3,4,0.5,7,8>");   // (left out)
        changed.replace("<R R1 1 140 150", "<R R1 1 160 150");
        changed.replace("<Components>\n", "<Components>\n  <L L9 1 500 500 10 -26 0 1 \"1 uH\" 1 \"\" 0>\n");
        changed.replace("<Wires>\n", "<Wires>\n  <500 470 600 470 \"probe\" 560 440 0 \"\">\n");
        QStringList lines = schematicChanges(base, changed);
        QVERIFY2(lines.contains("C1 (C): C \"10nF\" \u2192 \"33nF\""), qPrintable(lines.join(" | ")));
        QVERIFY2(lines.contains("R1 (R): moved from 140,150 to 160,150"), qPrintable(lines.join(" | ")));
        QVERIFY2(lines.contains("L9 (L) added at 500,500"), qPrintable(lines.join(" | ")));
        QVERIFY2(lines.contains("wires: 1 added"), qPrintable(lines.join(" | ")));
        QVERIFY2(lines.contains("labels: probe added"), qPrintable(lines.join(" | ")));
        QVERIFY(!lines.join(' ').contains("View"));
        QVERIFY(schematicChanges(base, QString(base).replace(QRegularExpression("<View=[^>]*>"), "<View=1,2,3,4,1,0,0>")).isEmpty());
        QCOMPARE(schematicChanges(std::nullopt, base).first().left(4), QStringLiteral("new:"));
        QCOMPARE(schematicChanges(base, std::nullopt), QStringList({"deleted"}));
        // An equation's variables by name.
        const QString eqn = "<Qucs Schematic 26.1.7>\n<Components>\n  <Eqn Eqn1 1 0 0 0 0 0 0 \"a=1\" 1 \"b=2\" 1 \"yes\" 0>\n</Components>\n";
        lines = schematicChanges(eqn, QString(eqn).replace("\"a=1\" 1 \"b=2\"", "\"b=3\" 1 \"c=4\""));
        QVERIFY2(lines.size() == 1 && lines.first().contains("a removed") && lines.first().contains("b \"2\" \u2192 \"3\"")
                     && lines.first().contains("c added"),
                 qPrintable(lines.join(" | ")));

        // In a repository: Claude's git_diff and git_show, the windows.
        const QString repo = makeRepo("sch-diff");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/RC_filter_FFT.sch", base.toUtf8()));
        QVERIFY(commit(repo, "filter", false, {repo + "/RC_filter_FFT.sch"}).ok());
        QVERIFY(write(repo + "/RC_filter_FFT.sch", QString(base).replace("\"10nF\"", "\"33nF\"").toUtf8()));
        QString diffText = text(call("git_diff", {{"path", repo}}));
        QVERIFY2(diffText.startsWith("Schematic changes, part by part:\nRC_filter_FFT.sch:\n  C1 (C): C \"10nF\" \u2192 \"33nF\""), qPrintable(diffText.left(300)));
        QVERIFY(diffText.contains("-  <C C1"));   // (git's lines after)
        QVERIFY(schematicChangesOf(repo, {}, DiffOf::Staged).isEmpty());
        QVERIFY(stage(repo, {repo + "/RC_filter_FFT.sch"}).ok());
        QVERIFY(schematicChangesOf(repo, {}, DiffOf::Staged).contains("33nF"));
        QVERIFY(schematicChangesOf(repo, {}, DiffOf::Unstaged).isEmpty());
        // Changed again after it was staged: staged, the index's; not staged, the file's.
        QVERIFY(write(repo + "/RC_filter_FFT.sch", QString(base).replace("\"10nF\"", "\"68nF\"").toUtf8()));
        QVERIFY2(schematicChangesOf(repo, {}, DiffOf::Staged).contains("\"10nF\" \u2192 \"33nF\""), qPrintable(schematicChangesOf(repo, {}, DiffOf::Staged)));
        QVERIFY(!schematicChangesOf(repo, {}, DiffOf::Staged).contains("68nF"));
        QVERIFY(schematicChangesOf(repo, {}, DiffOf::Unstaged).contains("\"33nF\" \u2192 \"68nF\""));
        QVERIFY(write(repo + "/RC_filter_FFT.sch", QString(base).replace("\"10nF\"", "\"33nF\"").toUtf8()));
        QVERIFY(commit(repo, "33nF").ok());
        const QString shown = text(call("git_show", {{"path", repo}, {"commit", "HEAD"}}));
        QVERIFY2(shown.startsWith("Schematic changes, part by part:") && shown.contains("\"33nF\""), qPrintable(shown.left(300)));
        QVERIFY(text(call("git_show", {{"path", repo}, {"commit", "HEAD~1"}})).contains("new: 8 parts"));
        HistoryDialog history(repo);
        history.commits()->setCurrentItem(history.commits()->topLevelItem(0));
        QTRY_VERIFY(history.details()->toPlainText().startsWith("Schematic changes, part by part:"));
        QVERIFY(write(repo + "/RC_filter_FFT.sch", QString(base).replace("\"10nF\"", "\"47nF\"").toUtf8()));
        Commands::instance()->commit(repo);
        CommitDialog* d = nullptr;
        for (const QPointer<QDialog>& w : Commands::instance()->openWindows())
            if (auto* c = qobject_cast<CommitDialog*>(w.data()); c != nullptr && c->root() == repo) d = c;
        QVERIFY(d != nullptr);
        d->unstagedList()->setCurrentRow(0);
        emit d->unstagedList()->itemClicked(d->unstagedList()->currentItem());
        QVERIFY2(d->diffView()->toPlainText().contains("C1 (C): C \"33nF\" \u2192 \"47nF\""), qPrintable(d->diffView()->toPlainText().left(300)));
        d->close();
    }

    // Claude's tools: the state, the changes, the history, blame; stage,

    // Claude's tools: the state, the changes, the history, blame; stage,
    // unstage, discard, commit (and push), branches, the remotes, stashes,
    // tags, a commit's actions, abort, init, clone, ignore. What would
    // throw work away, or publish it, is asked about each time.
    void claudesGitTools()
    {
        const QString repo = makeRepo("claude");
        QVERIFY(!repo.isEmpty());
        QVERIFY(write(repo + "/a.txt", "one\nTWO\nthree\n"));
        QVERIFY(write(repo + "/new.txt", "new\n"));
        const QString plain = canonical(dir.path()) + "/claude-plain";
        QVERIFY(QDir().mkpath(plain));

        // No repository: said, with what makes one.
        QJsonObject r = call("git_status", {{"path", plain}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("is in no git repository") && text(r).contains("git_init"), qPrintable(text(r)));

        r = call("git_status", {{"path", repo + "/a.txt"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject s = json(r);
        QCOMPARE(s.value("branch").toString(), QStringLiteral("main"));
        QCOMPARE(canonical(s.value("repository").toString()), repo);
        QCOMPARE(pathsIn(s.value("not staged").toArray()), QStringList({"a.txt"}));
        QCOMPARE(s.value("not staged").toArray().first().toObject().value("state").toString(), QStringLiteral("modified"));
        QCOMPARE(pathsIn(s.value("untracked").toArray()), QStringList({"new.txt"}));
        QVERIFY(s.value("staged").toArray().isEmpty());
        QCOMPARE(s.value("path").toObject().value("state").toString(), QStringLiteral("Modified, not staged"));
        QVERIFY(!s.contains("clean"));

        r = call("git_diff", {{"path", repo}});
        QVERIFY(text(r).contains("+TWO") && text(r).contains("+new"));
        QVERIFY(text(call("git_diff", {{"path", repo}, {"of", "staged"}})).startsWith("No changes"));
        QVERIFY(failed(call("git_diff", {{"path", repo}, {"of", "everything"}})));

        // Stage: paths relative to the repository named.
        QVERIFY(failed(call("git_stage", {{"path", repo}})));   // which files?
        r = call("git_stage", {{"path", repo}, {"paths", QJsonArray{"a.txt"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(pathsIn(json(r).value("staged").toArray()), QStringList({"a.txt"}));
        QVERIFY(text(call("git_diff", {{"path", repo}, {"of", "staged"}})).contains("+TWO"));
        r = call("git_unstage", {{"path", repo}, {"all", true}});
        QVERIFY(json(r).value("staged").toArray().isEmpty());
        r = call("git_stage", {{"paths", QJsonArray{repo + "/a.txt", repo + "/new.txt"}}});
        QCOMPARE(json(r).value("staged").toArray().size(), 2);
        r = call("git_unstage", {{"paths", QJsonArray{repo + "/new.txt"}}});
        QCOMPARE(pathsIn(json(r).value("untracked").toArray()), QStringList({"new.txt"}));
        // A path of another folder than the repository named: refused.
        r = call("git_stage", {{"path", repo}, {"paths", QJsonArray{plain + "/x.txt"}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("not in the repository"), qPrintable(text(r)));

        // Commit: nothing staged but 'paths' - those; then what is staged.
        r = call("git_commit", {{"path", repo}, {"message", "nothing"}, {"paths", QJsonArray()}});
        // (a.txt is staged: committed)
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("commit").toString().size(), 7);
        QCOMPARE(log(repo).first().subject, QStringLiteral("nothing"));
        r = call("git_commit", {{"path", repo}, {"message", "again"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("Nothing is staged"), qPrintable(text(r)));
        QVERIFY(failed(call("git_commit", {{"path", repo}, {"message", "  "}})));
        r = call("git_commit", {{"path", repo}, {"message", "the new file"}, {"paths", QJsonArray{"new.txt"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("clean").toBool());
        r = call("git_commit", {{"path", repo}, {"message", "the new file, amended"}, {"amend", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(log(repo).first().subject, QStringLiteral("the new file, amended"));

        // The history, a commit, blame.
        r = call("git_log", {{"path", repo}});
        QJsonArray commits = json(r).value("commits").toArray();
        QCOMPARE(commits.size(), 3);
        QCOMPARE(commits.first().toObject().value("subject").toString(), QStringLiteral("the new file, amended"));
        QCOMPARE(commits.first().toObject().value("author").toString(), QStringLiteral("Tester <tester@example.com>"));
        QCOMPARE(json(call("git_log", {{"path", repo}, {"max", 1}})).value("commits").toArray().size(), 1);
        QVERIFY(json(call("git_log", {{"path", repo}, {"max", 1}})).contains("more"));
        QCOMPARE(json(call("git_log", {{"path", repo + "/new.txt"}})).value("commits").toArray().size(), 1);
        QVERIFY(failed(call("git_log", {{"path", repo}, {"ref", "--output=" + repo + "/out"}})));
        QVERIFY(!QFileInfo::exists(repo + "/out"));
        r = call("git_show", {{"path", repo}, {"commit", "HEAD~1"}});
        QVERIFY2(text(r).contains("nothing") && text(r).contains("+TWO"), qPrintable(text(r)));
        QVERIFY(failed(call("git_show", {{"path", repo}, {"commit", "no-such-commit"}})));
        QVERIFY(failed(call("git_show", {{"path", repo}, {"commit", "--output=" + repo + "/out"}})));
        QVERIFY(!QFileInfo::exists(repo + "/out"));
        r = call("git_blame", {{"path", repo + "/a.txt"}, {"from", 2}, {"to", 2}});
        QJsonArray lines = json(r).value("lines").toArray();
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines.first().toObject().value("text").toString(), QStringLiteral("TWO"));
        QCOMPARE(lines.first().toObject().value("summary").toString(), QStringLiteral("nothing"));
        QVERIFY(failed(call("git_blame", {{"path", repo + "/none.txt"}})));

        // Discard: a.txt back, an untracked file to the trash.
        QVERIFY(write(repo + "/a.txt", "thrown away\n"));
        QVERIFY(write(repo + "/scratch.txt", "scratch\n"));
        QVERIFY(failed(call("git_discard", {{"path", repo}})));
        r = call("git_discard", {{"path", repo}, {"paths", QJsonArray{"a.txt", "scratch.txt"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\nTWO\nthree\n"));
        QVERIFY(!QFileInfo::exists(repo + "/scratch.txt"));
        QCOMPARE(json(r).value("to the trash").toArray().size(), 1);
        QVERIFY(json(r).value("clean").toBool());

        // Branches.
        r = call("git_branch", {{"path", repo}, {"action", "create"}, {"name", "topic"}});
        QCOMPARE(json(r).value("branch").toString(), QStringLiteral("topic"));
        QVERIFY(write(repo + "/a.txt", "one\ntopic\nthree\n"));
        QVERIFY(!failed(call("git_commit", {{"path", repo}, {"message", "on topic"}, {"paths", QJsonArray{"a.txt"}}})));
        r = call("git_branch", {{"path", repo}, {"action", "switch"}, {"name", "main"}});
        QCOMPARE(json(r).value("branch").toString(), QStringLiteral("main"));
        r = call("git_branch", {{"path", repo}});
        QJsonArray local = json(r).value("local").toArray();
        QCOMPARE(local.size(), 2);
        // A switch with changes in the way: refused, then stashed first.
        QVERIFY(write(repo + "/a.txt", "one\nin the way\nthree\n"));
        r = call("git_branch", {{"path", repo}, {"action", "switch"}, {"name", "topic"}});
        QVERIFY(failed(r));
        r = call("git_branch", {{"path", repo}, {"action", "switch"}, {"name", "topic"}, {"stash", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("branch").toString(), QStringLiteral("topic"));
        QCOMPARE(json(r).value("stashes").toInt(), 1);
        QVERIFY(!failed(call("git_branch", {{"path", repo}, {"action", "switch"}, {"name", "main"}})));
        r = call("git_stash", {{"path", repo}});
        QCOMPARE(json(r).value("stashes").toArray().size(), 1);
        QVERIFY(text(call("git_stash", {{"path", repo}, {"action", "show"}})).contains("+in the way"));
        r = call("git_stash", {{"path", repo}, {"action", "pop"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("one\nin the way\nthree\n"));
        // A document open of a file git changes: loaded again before the
        // tool answers (the next tool reads it as it is).
        QVERIFY(app->gotoPage(repo + "/a.txt"));
        auto* open = dynamic_cast<TextDoc*>(app->getDoc());
        QVERIFY(open != nullptr);
        QCOMPARE(open->toPlainText(), QStringLiteral("one\nin the way\nthree\n"));
        r = call("git_stash", {{"path", repo}, {"action", "push"}, {"message", "kept"}});
        QVERIFY(json(r).value("clean").toBool());
        open = dynamic_cast<TextDoc*>(app->getDoc());
        QVERIFY(open != nullptr);
        QCOMPARE(open->toPlainText(), QString::fromUtf8(readAll(repo + "/a.txt")));
        QVERIFY(!open->toPlainText().contains("in the way"));
        open->setDocChanged(false);
        app->closeAllFiles();
        QVERIFY(!failed(call("git_stash", {{"path", repo}, {"action", "drop"}})));
        QVERIFY(json(call("git_stash", {{"path", repo}})).value("stashes").toArray().isEmpty());
        QVERIFY(failed(call("git_stash", {{"path", repo}, {"action", "stow"}})));

        // A merge in conflict: said, not an error; given up.
        QVERIFY(write(repo + "/a.txt", "one\nmain side\nthree\n"));
        QVERIFY(!failed(call("git_commit", {{"path", repo}, {"message", "on main"}, {"paths", QJsonArray{"a.txt"}}})));
        r = call("git_branch", {{"path", repo}, {"action", "merge"}, {"name", "topic"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("under way").toString(), QStringLiteral("merge"));
        QCOMPARE(json(r).value("conflicts").toArray(), QJsonArray({"a.txt"}));
        QVERIFY(json(r).value("merge").toString().contains("git_abort"));
        r = call("git_abort", {{"path", repo}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!json(r).contains("under way"));
        QVERIFY(failed(call("git_abort", {{"path", repo}})));   // nothing under way
        // Delete: not merged refused, then forced; rename.
        r = call("git_branch", {{"path", repo}, {"action", "delete"}, {"name", "topic"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("'force'"), qPrintable(text(r)));
        QVERIFY(!failed(call("git_branch", {{"path", repo}, {"action", "rename"}, {"name", "topic"}, {"to", "topic2"}})));
        QVERIFY(!failed(call("git_branch", {{"path", repo}, {"action", "delete"}, {"name", "topic2"}, {"force", true}})));
        QCOMPARE(json(call("git_branch", {{"path", repo}})).value("local").toArray().size(), 1);
        QVERIFY(failed(call("git_branch", {{"path", repo}, {"action", "create"}, {"name", "--orphan"}})));
        QVERIFY(failed(call("git_branch", {{"path", repo}, {"action", "fly"}, {"name", "x"}})));

        // Tags; a commit's actions.
        r = call("git_tag", {{"path", repo}, {"action", "create"}, {"name", "v1"}, {"message", "the first"}});
        QCOMPARE(json(r).value("tags").toArray(), QJsonArray({"v1"}));
        QVERIFY(!failed(call("git_tag", {{"path", repo}, {"action", "delete"}, {"name", "v1"}})));
        QVERIFY(json(call("git_tag", {{"path", repo}})).value("tags").toArray().isEmpty());
        r = call("git_commit_action", {{"path", repo}, {"commit", "HEAD"}, {"action", "revert"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(log(repo).first().subject.startsWith("Revert"));
        r = call("git_commit_action", {{"path", repo}, {"commit", "HEAD~1"}, {"action", "reset"}, {"mode", "hard"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(log(repo).first().subject, QStringLiteral("on main"));
        r = call("git_commit_action", {{"path", repo}, {"commit", "HEAD~1"}, {"action", "check_out"}});
        QVERIFY(json(r).value("detached").toBool());
        QVERIFY(json(r).value("branch").isNull());
        QVERIFY(!failed(call("git_branch", {{"path", repo}, {"action", "switch"}, {"name", "main"}})));
        QVERIFY(failed(call("git_commit_action", {{"path", repo}, {"commit", "HEAD"}, {"action", "squash"}})));
        QVERIFY(failed(call("git_commit_action", {{"path", repo}, {"commit", "--hard"}, {"action", "reset"}})));

        // Ignore (and stop following); init; the remotes; clone; push.
        r = call("git_ignore", {{"path", repo + "/new.txt"}, {"untrack", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(readAll(repo + "/.gitignore").contains("/new.txt"));
        QVERIFY(QFileInfo::exists(repo + "/new.txt"));
        QCOMPARE(pathsIn(json(r).value("staged").toArray()), QStringList({"new.txt"}));   // deleted, staged
        QVERIFY(!failed(call("git_stage", {{"path", repo}, {"paths", QJsonArray{".gitignore"}}})));
        r = call("git_commit", {{"path", repo}, {"message", "new.txt ignored"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("clean").toBool());

        const QString made = canonical(dir.path()) + "/claude-init";
        QVERIFY(QDir().mkpath(made));
        r = call("git_init", {{"path", made}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("branch").toString(), QStringLiteral("main"));
        QVERIFY(failed(call("git_init", {{"path", made}})));   // one already
        QVERIFY(failed(call("git_init", {{"path", made + "/none"}})));

        const QString bare = canonical(dir.path()) + "/claude-remote.git";
        QVERIFY(QDir().mkpath(bare));
        QVERIFY(!gitIn(bare, {"init", "-q", "--bare"}).isNull());
        r = call("git_remote", {{"path", repo}, {"action", "push"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("no remote"), qPrintable(text(r)));
        QVERIFY(!failed(call("git_remote", {{"path", repo}, {"action", "add"}, {"name", "origin"}, {"url", bare}})));
        r = call("git_remote", {{"path", repo}});
        QCOMPARE(json(r).value("remotes").toArray().first().toObject().value("name").toString(), QStringLiteral("origin"));
        r = call("git_remote", {{"path", repo}, {"action", "push"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("upstream").toString(), QStringLiteral("origin/main"));
        QVERIFY(failed(call("git_remote", {{"path", repo}, {"action", "fetch"}, {"remote", "--upload-pack=touch " + repo + "/pwned"}})));
        QVERIFY(!QFileInfo::exists(repo + "/pwned"));
        QVERIFY(failed(call("git_remote", {{"path", repo}, {"action", "add"}, {"name", "evil"}, {"url", "--upload-pack=x"}})));
        QVERIFY(failed(call("git_remote", {{"path", repo}, {"action", "beam"}})));

        // A clone: into the workspace by default; a folder not empty refused.
        r = call("git_clone", {{"url", bare}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString cloned = QucsSettings.qucsWorkspaceDir.filePath("claude-remote");
        QCOMPARE(canonical(json(r).value("cloned into").toString()), canonical(cloned));
        QCOMPARE(readAll(cloned + "/a.txt"), QByteArray("one\nmain side\nthree\n"));
        QVERIFY(failed(call("git_clone", {{"url", bare}})));   // there already
        QVERIFY(failed(call("git_clone", {{"url", "--upload-pack=touch " + plain + "/pwned"}, {"path", plain + "/c"}})));
        QVERIFY(!QFileInfo::exists(plain + "/pwned"));
        // Committed there and pushed in one; fetched and pulled here.
        QVERIFY(write(cloned + "/a.txt", "from the clone\n"));
        r = call("git_commit", {{"path", cloned}, {"message", "from the clone"}, {"paths", QJsonArray{"a.txt"}}, {"push", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("pushed").toBool());
        r = call("git_remote", {{"path", repo}, {"action", "fetch"}});
        QCOMPARE(json(r).value("behind").toInt(), 1);
        r = call("git_remote", {{"path", repo}, {"action", "pull"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(readAll(repo + "/a.txt"), QByteArray("from the clone\n"));
        QVERIFY(!failed(call("git_remote", {{"path", repo}, {"action", "remove"}, {"name", "origin"}})));
        QVERIFY(json(call("git_remote", {{"path", repo}})).value("remotes").toArray().isEmpty());

        // Asked about each time: what loses work or publishes it.
        QVERIFY(control->irreversible("git_discard", {{"all", true}}));
        QVERIFY(control->irreversible("git_commit_action", {{"commit", "HEAD"}, {"action", "reset"}, {"mode", "hard"}}));
        QVERIFY(!control->irreversible("git_commit_action", {{"commit", "HEAD"}, {"action", "reset"}, {"mode", "soft"}}));
        QVERIFY(control->irreversible("git_branch", {{"action", "delete"}, {"name", "x"}, {"force", true}}));
        QVERIFY(!control->irreversible("git_branch", {{"action", "delete"}, {"name", "x"}}));
        QVERIFY(control->irreversible("git_stash", {{"action", "drop"}}));
        QVERIFY(control->irreversible("git_remote", {{"action", "push"}}));
        QVERIFY(control->irreversible("git_commit", {{"message", "m"}, {"push", true}}));
        QVERIFY(!control->irreversible("git_commit", {{"message", "m"}}));
        for (const char* ro : {"git_status", "git_diff", "git_log", "git_show", "git_blame"})
            QVERIFY2(control->readOnlyTools().contains(ro), ro);
        QVERIFY(!control->readOnlyTools().contains("git_stage"));
    }
};

QTEST_MAIN(TestGitIntegration)
#include "test_git_integration.moc"
