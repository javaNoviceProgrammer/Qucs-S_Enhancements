/*
 * The git integration fuzzed, three ways.
 *
 * fuzzTheReaders: what git writes, as real git wrote it for a repository of
 * awkward names, then damaged - read by every reader (status, log, refs,
 * remotes, stashes, a commit's files, blame, numstat), the commits laid out
 * as a graph and painted, a damaged schematic summed part by part. No crash,
 * no sanitizer's report, no reader that takes long, and what the windows
 * count on: a commit's hash a hash, a graph's lanes in its width, each entry
 * found by its own path.
 *
 * fuzzTheWindows: a git program that answers with such damaged texts, under
 * the History, the Commit, Blame and Changes windows and the menus.
 *
 * fuzzARepository: real git on a repository of hostile file and branch names
 * (spaces, lines, quotes, patterns, leading dashes, a leading colon, accents
 * in either form), driven by random edits, stages, unstages, discards,
 * commits, stashes and branches, against a model of what each file should be
 * in HEAD, in the index and on the disk. After each step every file on the
 * disk is as the model says - so an operation on one file that touches
 * another is caught at once - and every so often what read(), status(),
 * diff(), log() and fileAt() say is checked against it.
 *
 * QUCS_FUZZ_SEED (1), QUCS_FUZZ_STEPS (CI's short pass) or QUCS_FUZZ_SECONDS
 * (a long run: steps without limit until then), QUCS_FUZZ_TRACE (a file for
 * the steps; else in the test's folder). QUCS_FUZZ_ONLY keeps one fuzzer to
 * a run by its name, as a long run wants. A crash or a report ends the run;
 * the trace (each step, flushed) and the seed replay it.
 *
 * Every repository is in a temporary folder.
 */

#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTreeView>
#include <QTreeWidget>

#include <map>
#include <optional>

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

QByteArray gitIn(const QString& dir, const QStringList& args, const QByteArray& input = {})
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(realGit(), args);
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

bool executable(const QString& path, const QByteArray& text)
{
    return write(path, text) && QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// The way a name is compared: one spelling of its accents, where the file
// system takes both.
QString sameName(const QString& name)
{
#ifdef Q_OS_MACOS
    return name.normalized(QString::NormalizationForm_C);
#else
    return name;
#endif
}

// What the fuzz draws from.
class Draw
{
public:
    explicit Draw(quint32 seed) : r(seed) {}
    QRandomGenerator r;
    int below(int n) { return n <= 0 ? 0 : int(r.bounded(n)); }
    bool chance(int percent) { return below(100) < percent; }
    template <typename T>
    const T& pick(const QList<T>& list) { return list.at(below(int(list.size()))); }

    // A text damaged.
    QByteArray damaged(QByteArray t, const QList<QByteArray>& others)
    {
        static const QList<QByteArray> tokens{QByteArray(1, '\0'), "\x1e", "\x1f", "\n", "\t", " ", "%", "\r\n", "..", "/", "\xff", "\xc3",
                                              "-", "--output=/tmp/x", "1 ", "2 ", "u ", "? ", "! ", "# branch.head ", "# branch.oid ",
                                              "# branch.ab +99999999999 -x", "# branch.upstream ", "R100", "HEAD -> ", "tag: ", "refs/heads/",
                                              "refs/remotes/", "(fetch)", "(push)", "stash@{99999999999}", "stash@{-1}", "<<<<<<<\n", "=======\n",
                                              ">>>>>>>\n", "\xf0\x9f\x98\x80", "e\xcc\x81", QByteArray(40, 'a'), QByteArray(64, 'f'),
                                              "author ", "author-time 99999999999999999999", "summary ", "\t", "<Components>", "</Components>",
                                              "<R R1 1", "<Wires>", "<Diagrams>", "<Paintings>", "<Properties>", "\"", "<", ">", "99999999999999999999"};
        for (int n = 1 + below(4); n > 0; --n) {
            const int at = t.isEmpty() ? 0 : below(int(t.size()));
            switch (below(9)) {
            case 0:   // a byte changed
                if (!t.isEmpty()) t[at] = char(below(256));
                break;
            case 1:   // a byte in
                t.insert(at, char(below(256)));
                break;
            case 2:   // a token in
                t.insert(at, pick(tokens));
                break;
            case 3:   // a stretch out
                t.remove(at, below(1 + std::min<int>(int(t.size()) - at, 400)));
                break;
            case 4:   // a stretch twice
                t.insert(at, t.mid(at, below(400)));
                break;
            case 5:   // cut short
                if (chance(30)) t.truncate(at);
                break;
            case 6:   // a separator out, so that one field runs into the next
                if (const qsizetype nul = t.indexOf('\0', at); nul >= 0) t.remove(nul, 1);
                break;
            case 7:   // part of another text
                if (!others.isEmpty()) {
                    const QByteArray& o = pick(others);
                    t.insert(at, o.mid(below(int(o.size())), below(600)));
                }
                break;
            case 8:   // much of it
                if (chance(5) && t.size() < 60000) t = t.repeated(1 + below(4));
                break;
            }
        }
        return t.left(256 * 1024);
    }
};

// A name no file system here refuses, and git takes.
QString hostileName(Draw& d, int n)
{
    static const QStringList pieces{QStringLiteral(" "), QStringLiteral("  "), QStringLiteral("\t"), QStringLiteral("\n"), QStringLiteral("\""),
                                    QStringLiteral("'"), QStringLiteral("\\"), QStringLiteral("*"), QStringLiteral("?"), QStringLiteral("["),
                                    QStringLiteral("]"), QStringLiteral("[ab]"), QStringLiteral(":"), QStringLiteral(":!"), QStringLiteral(":(top)"),
                                    QStringLiteral("!"), QStringLiteral("#"), QStringLiteral("%1"), QStringLiteral("%"), QStringLiteral("&"),
                                    QStringLiteral(";"), QStringLiteral("|"), QStringLiteral("$"), QStringLiteral("$(x)"), QStringLiteral("<"),
                                    QStringLiteral(">"), QStringLiteral("("), QStringLiteral(")"), QStringLiteral("-"), QStringLiteral("--"),
                                    QStringLiteral("~"), QStringLiteral("^"), QStringLiteral("{"), QStringLiteral("}"), QStringLiteral("@{0}"),
                                    QStringLiteral(".."), QStringLiteral("."), QStringLiteral("HEAD"), QStringLiteral("caf\u00e9"), QStringLiteral("cafe\u0301"),
                                    QStringLiteral("\u0645\u06cc\u062b\u0645"), QStringLiteral("\u4e2d\u6587"), QStringLiteral("\U0001f600"),
                                    QStringLiteral("\u00b5\u03a9"), QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("R1"), QStringLiteral(".sch"),
                                    QStringLiteral("\x7f"), QStringLiteral("\x1f"), QStringLiteral("\x1e"), QStringLiteral("\x01")};
    QString name;
    for (int k = 1 + d.below(5); k > 0; --k) name += pieces.at(d.below(int(pieces.size())));
    // (Unique whatever the disk's case and accents do.)
    name += QStringLiteral("_%1").arg(n);
    switch (d.below(6)) {
    case 0: name.prepend(QLatin1Char('-')); break;
    case 1: name.prepend(QLatin1Char(':')); break;
    case 2: name.append(QLatin1Char(' ')); break;
    default: break;
    }
    return name;
}

} // namespace

class TestGitFuzz : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    int made = 0;
    // What real git wrote.
    QList<QByteArray> statusOut, logOut, branchOut, remoteOut, stashOut, blameOut, namesOut, countsOut, numstatOut, blobsOut, schematics;

    QString folder(const QString& name)
    {
        const QString path = canonical(dir.path()) + "/" + name + QString::number(++made);
        QDir().mkpath(path);
        return canonical(path);
    }

    bool only(const char* name) const
    {
        return !qEnvironmentVariableIsSet("QUCS_FUZZ_ONLY") || qEnvironmentVariable("QUCS_FUZZ_ONLY") == QLatin1String(name);
    }

    struct Plan {
        quint32 seed = 1;
        int seconds = 0;
        int steps = 0;
    };

    Plan plan(int shortPass) const
    {
        Plan p;
        p.seed = qEnvironmentVariableIsSet("QUCS_FUZZ_SEED") ? quint32(qEnvironmentVariableIntValue("QUCS_FUZZ_SEED")) : 1;
        p.seconds = qEnvironmentVariableIntValue("QUCS_FUZZ_SECONDS");
        p.steps = p.seconds > 0 ? INT_MAX : (qEnvironmentVariableIsSet("QUCS_FUZZ_STEPS") ? qEnvironmentVariableIntValue("QUCS_FUZZ_STEPS") : shortPass);
        return p;
    }

    // Real git's texts, of a repository with a bit of everything.
    bool gatherTexts()
    {
        const QString repo = folder("corpus");
        if (gitIn(repo, {"init", "-q"}).isNull()) return false;
        Draw d(7);
        QStringList names;
        for (int i = 0; i < 12; ++i) names << hostileName(d, i);
        for (const QString& n : names)
            if (!write(repo + "/" + n, "one\ntwo\n")) return false;
        QStringList sch = QDir(QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice").entryList({"*.sch"}, QDir::Files);
        for (const QString& s : sch.mid(0, 6)) schematics << readAll(QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/" + s);
        if (schematics.isEmpty()) schematics << "<Qucs Schematic 26.1.1>\n<Components>\n</Components>\n";
        write(repo + "/x.sch", schematics.first());
        write(repo + "/.gitignore", "*.ignored\n");
        write(repo + "/a.ignored", "x\n");
        gitIn(repo, {"add", "-A"});
        gitIn(repo, {"commit", "-q", "-m", "first\n\nits body"});
        gitIn(repo, {"tag", "-a", "-m", "m", "v1"});
        gitIn(repo, {"remote", "add", "origin", "https://example.invalid/x.git"});
        gitIn(repo, {"update-ref", "refs/remotes/origin/main", "HEAD"});
        gitIn(repo, {"branch", "--set-upstream-to=origin/main"});
        gitIn(repo, {"checkout", "-q", "-b", "side"});
        write(repo + "/" + names.at(0), "side\n");
        gitIn(repo, {"commit", "-q", "-a", "-m", "side's"});
        gitIn(repo, {"checkout", "-q", "main"});
        write(repo + "/" + names.at(1), "main\n");
        gitIn(repo, {"commit", "-q", "-a", "-m", "main's"});
        gitIn(repo, {"merge", "-q", "--no-edit", "side"});
        gitIn(repo, {"mv", "--", names.at(2), "moved"});
        write(repo + "/" + names.at(3), "changed\nagain\n");
        gitIn(repo, {"commit", "-q", "-a", "-m", "a rename"});
        write(repo + "/" + names.at(4), "stashed\n");
        gitIn(repo, {"stash", "push", "-q", "-m", "stashed"});
        write(repo + "/" + names.at(5), "not staged\n");
        write(repo + "/" + names.at(6), "staged\n");
        gitIn(repo, {"add", "--", ":(literal)" + names.at(6)});
        write(repo + "/new one", "new\n");
        gitIn(repo, {"mv", "--", names.at(7), "moved too"});
        const QString blamed = repo + "/" + names.at(3);

        // As the code asks: a git of its own that keeps what it writes.
        const QString shim = folder("shim") + "/git.sh";
        const QString kept = folder("kept");
        const QByteArray script = "#!/bin/sh\nword=\nskip=\nfor a in \"$@\"; do\n  if [ -n \"$skip\" ]; then skip=; continue; fi\n"
                                  "  case \"$a\" in -c) skip=1 ;; -*) ;; *) word=\"$a\"; break ;; esac\ndone\n'"
                                  + QFile::encodeName(realGit()) + "' \"$@\" | tee '" + QFile::encodeName(kept) + "/'\"$FUZZ_TAG.$word\"\n";
        if (!executable(shim, script)) return false;
        qputenv("QUCS_GIT", QFile::encodeName(shim));
        const auto tagged = [&](const char* tag, const std::function<void()>& work, QList<QByteArray>& into, const char* word) {
            qputenv("FUZZ_TAG", tag);
            work();
            into << readAll(kept + "/" + tag + "." + word);
        };
        tagged("read", [&] { read(repo); }, statusOut, "status");
        tagged("log", [&] { log(repo); }, logOut, "log");
        tagged("history", [&] { history(repo, HistoryQuery()); }, logOut, "log");
        tagged("branches", [&] { branches(repo); }, branchOut, "for-each-ref");
        tagged("remotes", [&] { remotes(repo); }, remoteOut, "remote");
        tagged("stashes", [&] { stashes(repo); }, stashOut, "stash");
        tagged("blame", [&] { blame(repo, blamed); }, blameOut, "blame");
        qunsetenv("QUCS_GIT");
        qunsetenv("FUZZ_TAG");
        const QStringList common{"diff-tree", "-r", "-M", "--no-commit-id", "-z"};
        // (A merge says nothing to plain diff-tree.)
        for (const char* commit : {"HEAD", "HEAD~2", "HEAD~3"}) {
            const QByteArray names = run(repo, common + QStringList{"--name-status", "--root", commit}, true).out.toUtf8();
            if (names.isEmpty()) continue;
            namesOut << names;
            countsOut << run(repo, common + QStringList{"--numstat", "--root", commit}, true).out.toUtf8();
        }
        if (namesOut.isEmpty()) {
            qWarning() << "no commit's files";
            return false;
        }
        numstatOut << gitIn(repo, {"diff", "--numstat", "--no-renames", "-z", "HEAD~1", "--"});
        blobsOut << gitIn(repo, {"cat-file", "--batch"}, "HEAD:x.sch\nHEAD:no such file\nHEAD:moved\nHEAD\n:x.sch\n");
        int which = 0;
        for (const auto* list : {&statusOut, &logOut, &branchOut, &remoteOut, &stashOut, &blameOut, &namesOut, &countsOut, &numstatOut}) {
            ++which;
            for (const QByteArray& text : *list)
                if (text.isEmpty()) {
                    qWarning() << "real git wrote nothing for kind" << which;
                    return false;
                }
        }
        return true;
    }

private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        QSKIP("names and programs here are not Windows' own");
#endif
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        if (program().isEmpty()) QSKIP("git is not installed");
        qputenv("QUCS_TRASH_DIR", QFile::encodeName(dir.filePath("trash")));
        qputenv("GIT_CEILING_DIRECTORIES", QFile::encodeName(canonical(dir.path())) + QByteArray(1, QDir::listSeparator().toLatin1())
                                               + QFile::encodeName(dir.path()));
        // (Renames off: the model has each path to itself.)
        QVERIFY(write(dir.filePath("gitconfig"),
                      "[user]\n\tname = Tester\n\temail = tester@example.com\n[init]\n\tdefaultBranch = main\n"
                      "[commit]\n\tgpgsign = false\n[tag]\n\tgpgsign = false\n[status]\n\trenames = false\n[diff]\n\trenames = false\n"
                      "[core]\n\tautocrlf = false\n[gc]\n\tauto = 0\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(dir.filePath("gitconfig")));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        QVERIFY(gatherTexts());
    }

    void fuzzTheReaders()
    {
        if (!only("readers")) QSKIP("QUCS_FUZZ_ONLY");
        const Plan p = plan(3000);
        Draw d(p.seed);
        QFile trace(qEnvironmentVariableIsSet("QUCS_FUZZ_TRACE") ? qEnvironmentVariable("QUCS_FUZZ_TRACE") + ".readers" : dir.filePath("trace-readers.txt"));
        QVERIFY(trace.open(QIODevice::WriteOnly));
        QList<QByteArray> all;
        for (const auto* list : {&statusOut, &logOut, &branchOut, &remoteOut, &stashOut, &blameOut, &namesOut, &countsOut, &numstatOut, &blobsOut, &schematics}) all += *list;
        QElapsedTimer clock;
        clock.start();
        QPixmap canvas(900, 40);
        GraphDelegate delegate;
        qint64 slowest = 0;
        int step = 0;
        for (; step < p.steps && (p.seconds == 0 || clock.elapsed() < p.seconds * 1000); ++step) {
            const int kind = d.below(12);
            trace.write(QByteArray::number(step) + " " + QByteArray::number(kind) + "\n");
            trace.flush();
            QElapsedTimer one;
            one.start();
            const auto text = [&](const QList<QByteArray>& list) { return QString::fromUtf8(d.damaged(d.pick(list), all)); };
            switch (kind) {
            case 0:
            case 1: {
                Repository r;
                readStatus(text(statusOut), r);
                for (const Entry& e : r.entries) {
                    QVERIFY2(r.entryOf(e.path) != nullptr, qPrintable(e.path));
                    (void)e.letter();
                    (void)e.describe();
                    (void)r.changedIn(e.path);
                }
                (void)r.branchText();
                QVERIFY(r.changedCount() >= r.conflictCount());
                QVERIFY(r.entryOf(QStringLiteral("no/such/file")) == nullptr || !r.entries.isEmpty());
                break;
            }
            case 2:
            case 3:
            case 4: {
                const QList<Commit> commits = readCommits(text(logOut));
                GraphLayout layout;
                QVector<HistoryModel::Row> rows;
                for (const Commit& c : commits) {
                    QVERIFY2(c.hash.size() == 40 || c.hash.size() == 64, qPrintable(c.hash));
                    for (const QString& parent : c.parents) QVERIFY2(parent.size() == 40 || parent.size() == 64, qPrintable(parent));
                    const GraphRow row = layout.next(c.hash, c.parents, d.chance(5));
                    QVERIFY(row.lane >= 0 && row.lane < row.width);
                    QVERIFY(row.colour >= 0);
                    for (const QVector<GraphLine>* lines : {&row.above, &row.below})
                        for (const GraphLine& l : *lines) {
                            QVERIFY(l.from >= 0 && l.from < std::max(row.width, row.lane + 1) + 1);
                            QVERIFY(l.to >= 0 && l.to < std::max(row.width, row.lane + 1) + 1);
                            QVERIFY(l.colour >= 0);
                        }
                    HistoryModel::Row r;
                    r.commit = c;
                    r.graph = row;
                    r.head = d.chance(10);
                    rows << r;
                }
                QVERIFY(layout.openLanes() >= 0);
                // Drawn: names, initials and labels, whatever they hold.
                if (step % 16 == 0 && !rows.isEmpty()) {
                    HistoryModel model;
                    model.append(rows);
                    QPainter painter(&canvas);
                    for (int i = 0; i < std::min<int>(model.rowCount(), 30); ++i)
                        for (int c = 0; c < model.columnCount(); ++c) {
                            QStyleOptionViewItem option;
                            option.rect = QRect(0, 0, 900, 24);
                            option.state = d.chance(20) ? QStyle::State_Selected : QStyle::State_Enabled;
                            option.fontMetrics = QFontMetrics(option.font);
                            delegate.paint(&painter, option, model.index(i, c));
                            (void)model.data(model.index(i, c), Qt::ToolTipRole);
                        }
                    QVERIFY(model.rowOf(rows.first().commit.hash) >= 0);
                }
                break;
            }
            case 5:
                for (const Branch& b : readBranches(text(branchOut))) (void)b.date.isValid();
                break;
            case 6:
                for (const Remote& r : readRemotes(text(remoteOut))) QVERIFY(!r.name.contains(QLatin1Char(' ')));
                break;
            case 7:
                for (const Stash& s : readStashes(text(stashOut))) (void)s.message.size();
                break;
            case 8:
                for (const BlameLine& l : readBlame(text(blameOut))) QVERIFY(l.line >= 0);
                break;
            case 9: {
                for (const ChangedFile& f : readChangedFiles(text(namesOut), text(countsOut))) QVERIFY(!f.status.isNull());
                for (const FileChange& f : readNumstat(d.damaged(d.pick(numstatOut), all))) (void)f.path.size();
                break;
            }
            case 11: {
                const int count = d.below(8);
                const auto found = readBlobs(d.damaged(d.pick(blobsOut), all), count);
                QVERIFY(found.size() <= count);
                break;
            }
            case 10: {
                // A schematic's text, as a commit may hold it.
                const QString before = QString::fromUtf8(d.damaged(d.pick(schematics), all));
                const QString after = QString::fromUtf8(d.damaged(d.pick(schematics), all));
                (void)schematicChanges(before, after);
                (void)schematicChanges(std::nullopt, after);
                (void)schematicChanges(before, std::nullopt);
                break;
            }
            }
            const qint64 took = one.elapsed();
            slowest = std::max(slowest, took);
            // (Longer than this on a text of 256 KB is a reader that goes
            // quadratic, whatever the machine.)
            QVERIFY2(took < 20000, qPrintable(QStringLiteral("step %1, kind %2: %3 ms").arg(step).arg(kind).arg(took)));
        }
        qInfo().noquote() << QStringLiteral("readers: seed %1, %2 steps in %3 s, the slowest %4 ms").arg(p.seed).arg(step).arg(clock.elapsed() / 1000).arg(slowest);
    }

    void fuzzTheWindows()
    {
        if (!only("windows")) QSKIP("QUCS_FUZZ_ONLY");
        const Plan p = plan(6);
        Draw d(p.seed);
        QFile trace(qEnvironmentVariableIsSet("QUCS_FUZZ_TRACE") ? qEnvironmentVariable("QUCS_FUZZ_TRACE") + ".windows" : dir.filePath("trace-windows.txt"));
        QVERIFY(trace.open(QIODevice::WriteOnly));
        // A repository to the eye; a git that answers from files.
        const QString root = folder("fake");
        QVERIFY(gitIn(root, {"init", "-q"}).size() >= 0);
        QVERIFY(write(root + "/a.sch", schematics.first()));
        const QString answers = folder("answers");
        const QString shim = answers + "/git.sh";
        QVERIFY(executable(shim, "#!/bin/sh\nword=\nskip=\nfor a in \"$@\"; do\n  if [ -n \"$skip\" ]; then skip=; continue; fi\n"
                                 "  case \"$a\" in -c) skip=1 ;; -*) ;; *) word=\"$a\"; break ;; esac\ndone\ncd '" + QFile::encodeName(answers)
                                 + "' || exit 9\n[ -f \"$word.out\" ] && cat \"$word.out\"\n[ -f \"$word.rc\" ] && exit $(cat \"$word.rc\")\nexit 0\n"));
        qputenv("QUCS_GIT", QFile::encodeName(shim));
        QList<QByteArray> all;
        for (const auto* list : {&statusOut, &logOut, &branchOut, &remoteOut, &stashOut, &blameOut, &namesOut, &countsOut, &numstatOut}) all += *list;
        const QByteArray hash = QByteArray(40, 'a');
        QElapsedTimer clock;
        clock.start();
        int step = 0;
        for (; step < p.steps && (p.seconds == 0 || clock.elapsed() < p.seconds * 1000); ++step) {
            trace.write(QByteArray::number(step) + "\n");
            trace.flush();
            const auto answer = [&](const char* word, const QByteArray& text, int rc = 0) {
                write(answers + "/" + word + ".out", text);
                write(answers + "/" + word + ".rc", QByteArray::number(rc));
            };
            const auto failing = [&](int percent) { return d.chance(percent) ? 1 + d.below(128) : 0; };
            answer("status", d.damaged(d.pick(statusOut), all), failing(5));
            answer("log", d.damaged(d.pick(logOut), all), failing(5));
            answer("for-each-ref", d.damaged(d.pick(branchOut), all), failing(5));
            answer("remote", d.damaged(d.pick(remoteOut), all), failing(5));
            answer("stash", d.damaged(d.pick(stashOut), all), failing(5));
            answer("blame", d.damaged(d.pick(blameOut), all), failing(5));
            answer("rev-parse", d.chance(70) ? hash + "\n" : d.damaged(hash, all), failing(5));
            answer("symbolic-ref", d.damaged("main\n", all), failing(30));
            answer("diff-tree", d.damaged(d.pick(d.chance(50) ? namesOut : countsOut), all), failing(5));
            answer("diff", d.damaged(d.pick(d.chance(50) ? numstatOut : blameOut), all), failing(5));
            answer("show", d.damaged(d.pick(schematics), all), failing(5));
            answer("cat-file", d.damaged(d.pick(d.chance(50) ? schematics : blobsOut), all), failing(5));
            answer("ls-files", d.damaged(d.pick(namesOut), all), failing(5));
            answer("tag", d.damaged("v1\nv2\n", all), failing(5));
            answer("merge-base", hash + "\n", failing(20));
            answer("rev-list", d.damaged("1\t2\n", all), failing(20));
            answer("config", QByteArray(), 1);

            // The tracker and the commands' menus.
            const Repository* repo = Tracker::instance()->readNow(root);
            if (repo != nullptr) (void)repo->branchText();
            {
                QMenu menu;
                Commands* commands = Commands::instance();
                commands->fillFileMenu(&menu, root + "/a.sch");
                commands->fillEntryMenu(&menu, {root + "/a.sch"});
                commands->fillBranchMenu(&menu, root);
                commands->fillStashMenu(&menu, root);
            }
            // The History: rows chosen, found, their menus.
            {
                HistoryView view(root);
                view.resize(900, 500);
                view.show();
                auto* tree = view.findChild<QTreeView*>("gitCommits");
                const int rows = tree != nullptr && tree->model() != nullptr ? tree->model()->rowCount() : 0;
                for (int k = 0; k < 3 && rows > 0; ++k) {
                    view.selectRow(d.below(rows));
                    QCoreApplication::processEvents();
                    delete view.menuFor(d.below(rows + 2) - 1);
                    if (auto* files = view.findChild<QTreeWidget*>("gitCommitFiles"); files != nullptr && files->topLevelItemCount() > 0) {
                        files->setCurrentItem(files->topLevelItem(d.below(files->topLevelItemCount())));
                        delete view.fileMenuFor(0);
                    }
                }
                view.find(QStringLiteral("a"));
                view.findNext(d.chance(50));
                (void)view.grab();
                view.refresh();
                view.loadMore();
                view.setScope(QStringLiteral("current"));
                view.reload();
            }
            // A file's history, its blame, the Commit window's lists.
            {
                HistoryView view(root, root + "/a.sch");
                view.loadMore();
            }
            {
                BlameDialog blame(root, root + "/a.sch");
                (void)blame.grab();
            }
            {
                CommitDialog commit(root);
                for (QListWidget* list : commit.findChildren<QListWidget*>()) {
                    if (list->count() > 0) list->setCurrentRow(d.below(list->count()));
                    QCoreApplication::processEvents();
                }
                commit.reload();
            }
            // The Claude Code dock's bar's window.
            {
                const Status s = status(root);
                GitChangesDialog changes(s);
                auto* files = changes.findChild<QListWidget*>("gitChangesFiles");
                if (files != nullptr && files->count() > 0) files->setCurrentRow(d.below(files->count()));
                (void)qucs_s::git::diff(s, QStringLiteral("a.sch"));
            }
            if (!qucs_s::git::diff(root).isNull()) (void)show(root, "HEAD");
            (void)conflictVersions(root, root + "/a.sch");
            (void)schematicChangesOf(root);
            (void)schematicChangesIn(root, "HEAD");
            (void)pushArgs(read(root));
        }
        qunsetenv("QUCS_GIT");
        qInfo().noquote() << QStringLiteral("windows: seed %1, %2 rounds in %3 s").arg(p.seed).arg(step).arg(clock.elapsed() / 1000);
    }

    void fuzzARepository()
    {
        if (!only("repository")) QSKIP("QUCS_FUZZ_ONLY");
        const Plan p = plan(250);
        Draw d(p.seed);
        QFile trace(qEnvironmentVariableIsSet("QUCS_FUZZ_TRACE") ? qEnvironmentVariable("QUCS_FUZZ_TRACE") + ".repository" : dir.filePath("trace-repository.txt"));
        QVERIFY(trace.open(QIODevice::WriteOnly));
        QElapsedTimer clock;
        clock.start();
        int step = 0, checks = 0, repositories = 0;
        while (step < p.steps && (p.seconds == 0 || clock.elapsed() < p.seconds * 1000)) {
            // A repository a while: its first commit, and later ones with much in them.
            const QString repo = folder("model");
            ++repositories;
            QVERIFY(init(repo).ok());
            struct File {
                std::optional<QByteArray> head, index, work;
                QString lastCommit;
            };
            std::map<QString, File> files;
            for (int i = 0; i < 16; ++i) {
                QString name = hostileName(d, i);
                if (i >= 13) name = (d.chance(50) ? QStringLiteral("fold[1]/") : QStringLiteral("-d ir/")) + name;
                files[name] = File();
            }
            QStringList names;
            for (const auto& [name, f] : files) names << name;
            bool hasHead = false;
            int content = 0, commits = 0;
            QString lastSubject;
            const auto say = [&](const QString& what) {
                trace.write(QStringLiteral("%1 %2\n").arg(step).arg(what).toUtf8());
                trace.flush();
                return QStringLiteral("seed %1 step %2: %3 in %4").arg(p.seed).arg(step).arg(what, repo);
            };
            // Every file on the disk as the model says.
            const auto onDiskIsRight = [&](const QString& after) -> QString {
                for (const auto& [name, f] : files) {
                    const QString path = repo + "/" + name;
                    const bool there = QFileInfo::exists(path);
                    if (there != f.work.has_value()) return after + ": " + name + (there ? " is there and should not be" : " is gone and should be there");
                    if (there && readAll(path) != *f.work) return after + ": " + name + " changed, another's operation";
                }
                return {};
            };
            // What the integration says, by the model.
            const auto saysRight = [&](const QString& after) -> QString {
                const Repository r = read(repo);
                if (!r.valid) return after + ": read() is not valid";
                if (hasHead == r.head.isEmpty()) return after + ": HEAD";
                bool anything = false;
                for (const auto& [name, f] : files) {
                    const Entry* e = r.entryOf(name);
                    const bool staged = f.index != f.head, unstaged = f.work != f.index;
                    const bool untracked = !f.head && !f.index && f.work;
                    if (!f.head && !f.index && !f.work) {
                        if (e != nullptr && !e->isFolder()) return after + ": " + name + " has an entry and is nowhere";
                        continue;
                    }
                    if (f.work != f.head) anything = true;
                    if (!staged && !unstaged) {
                        if (e != nullptr) return after + ": " + name + " is as committed: " + e->describe();
                        continue;
                    }
                    anything = true;
                    if (e == nullptr) return after + ": " + name + " has no entry";
                    if (untracked) {
                        if (!e->untracked) return after + ": " + name + " is untracked: " + e->describe();
                        continue;
                    }
                    // (Out of the index, a file of that name there: git lists it twice.)
                    if (f.head && !f.index && f.work) continue;
                    if (e->hasStaged() != staged) return after + ": " + name + " staged: " + e->describe();
                    if ((e->unstaged != QLatin1Char('.')) != unstaged) return after + ": " + name + " not staged: " + e->describe();
                }
                if (anything != (r.changedCount() > 0)) return after + ": changedCount " + QString::number(r.changedCount());
                // Its changes since HEAD, counted (the Claude Code dock's bar).
                const Status s = status(repo);
                if (!s.repository) return after + ": status()";
                QSet<QString> changed;
                for (const FileChange& c : s.changes) changed.insert(sameName(c.path));
                for (const auto& [name, f] : files) {
                    const bool should = f.work != f.head && !(f.work && !f.head && !f.index && false);
                    if (hasHead && should != changed.contains(sameName(name))) return after + ": status() of " + name + (should ? " lacks it" : " has it");
                }
                if (anything && !s.dirty) return after + ": status() not dirty";
                // A file: its diff, its commit's version, its history, its blame.
                const QString name = names.at(d.below(int(names.size())));
                const File& f = files[name];
                if (f.work && f.work != f.head) {
                    const QString text = diff(repo, name, DiffOf::Head);
                    if (!text.contains(QLatin1Char('+') + QString::fromUtf8(f.work->left(f.work->indexOf('\n'))))) return after + ": diff of " + name + ": " + text.left(200);
                }
                if (hasHead) {
                    bool ok = false;
                    const QByteArray at = fileAt(repo, QStringLiteral("HEAD"), name, &ok);
                    if (ok != f.head.has_value() || (ok && at != *f.head)) return after + ": fileAt of " + name;
                    if (f.head) {
                        const QList<Commit> history = log(repo, name, 3);
                        if (history.isEmpty() || history.first().subject != f.lastCommit) return after + ": log of " + name;
                        if (f.work == f.head && QString(blame(repo, name).value(0).summary) != f.lastCommit) return after + ": blame of " + name;
                    }
                }
                return {};
            };
            for (int ops = 0; ops < 120 && step < p.steps && (p.seconds == 0 || clock.elapsed() < p.seconds * 1000); ++ops, ++step) {
                const QString name = names.at(d.below(int(names.size())));
                File& f = files[name];
                QString after;
                switch (d.below(13)) {
                case 0:
                case 1:
                case 2: {
                    const QByteArray text = "content " + QByteArray::number(++content) + "\nline\n";
                    after = say("edit " + name);
                    QVERIFY2(write(repo + "/" + name, text), qPrintable(after));
                    f.work = text;
                    break;
                }
                case 3:
                    if (!f.work) continue;
                    after = say("remove " + name);
                    QVERIFY2(QFile::remove(repo + "/" + name), qPrintable(after));
                    f.work.reset();
                    break;
                case 4:
                case 5: {
                    if (f.work == f.index) continue;
                    after = say("stage " + name);
                    const Result r = stage(repo, {name});
                    QVERIFY2(r.ok(), qPrintable(after + ": " + r.error()));
                    f.index = f.work;
                    break;
                }
                case 6: {
                    if (f.index == f.head) continue;
                    after = say("unstage " + name);
                    const Result r = unstage(repo, {name});
                    QVERIFY2(r.ok(), qPrintable(after + ": " + r.error()));
                    f.index = f.head;
                    break;
                }
                case 7: {
                    if (f.work == f.head && f.index == f.head) continue;
                    after = say("discard " + name);
                    QStringList trashed;
                    const Result r = discard(repo, {name}, &trashed);
                    QVERIFY2(r.ok(), qPrintable(after + ": " + r.error()));
                    f.work = f.index = f.head;
                    break;
                }
                case 8: {
                    // Commit that file alone, what else is staged left so.
                    if (f.work == f.head) continue;
                    after = say("commit " + name);
                    const QString subject = QStringLiteral("commit %1").arg(++commits);
                    const Result r = commit(repo, subject, false, {name});
                    QVERIFY2(r.ok(), qPrintable(after + ": " + r.error()));
                    f.head = f.index = f.work;
                    f.lastCommit = subject;
                    lastSubject = subject;
                    hasHead = true;
                    const QList<ChangedFile> changed = changedFiles(repo, QStringLiteral("HEAD"));
                    QVERIFY2(changed.size() == 1 && sameName(changed.first().path) == sameName(name), qPrintable(after));
                    break;
                }
                case 9: {
                    // Everything.
                    bool anything = false;
                    for (const auto& [n, x] : files) anything = anything || x.work != x.head;
                    if (!anything) continue;
                    after = say("stage all and commit");
                    QVERIFY2(stage(repo, {}).ok(), qPrintable(after));
                    const QString subject = QStringLiteral("commit %1").arg(++commits);
                    const Result r = commit(repo, subject);
                    QVERIFY2(r.ok(), qPrintable(after + ": " + r.error()));
                    for (auto& [n, x] : files) {
                        if (x.work != x.head) x.lastCommit = subject;
                        x.head = x.index = x.work;
                    }
                    lastSubject = subject;
                    hasHead = true;
                    break;
                }
                case 10: {
                    // A branch or tag of a hostile name: refused or whole.
                    if (!hasHead) continue;
                    const QString branch = hostileName(d, 100 + ops).replace(QLatin1Char('\n'), QLatin1Char('n'));
                    after = say("branch " + branch);
                    const bool valid = !branch.startsWith(QLatin1Char('-')) && !gitIn(repo, {"check-ref-format", "--branch", "x" + branch}).isNull();
                    const Result r = createBranch(repo, branch, {}, false);
                    if (branch.startsWith(QLatin1Char('-'))) QVERIFY2(!r.ok(), qPrintable(after));
                    if (r.ok() && valid) {
                        bool found = false;
                        for (const Branch& b : branches(repo)) found = found || sameName(b.name) == sameName(branch);
                        QVERIFY2(found, qPrintable(after));
                        QVERIFY2(!log(repo, {}, 1, QStringLiteral("refs/heads/") + branch).isEmpty(), qPrintable(after));
                        QVERIFY2(deleteBranch(repo, branch, true).ok(), qPrintable(after));
                    }
                    break;
                }
                case 11: {
                    // Stashed and back: the files as they were.
                    if (!hasHead) continue;
                    bool anything = false, odd = false;
                    for (const auto& [n, x] : files) {
                        anything = anything || x.work != x.head || x.index != x.head;
                        // (Out of the index with a file of that name on the disk,
                        // or staged as new with none: a stash of its own rules.)
                        odd = odd || (x.head && !x.index && x.work) || (!x.head && x.index && !x.work);
                    }
                    if (!anything || odd) continue;
                    after = say("stash and back");
                    const Result s = stash(repo, QStringLiteral("kept"), true);
                    QVERIFY2(s.ok(), qPrintable(after + ": " + s.error()));
                    QCOMPARE(stashes(repo).size(), qsizetype(1));
                    for (auto& [n, x] : files) {
                        const QFileInfo there(repo + "/" + n);
                        if (x.head) QVERIFY2(there.exists() && readAll(there.filePath()) == *x.head, qPrintable(after + ": stashed " + n));
                        else QVERIFY2(!there.exists(), qPrintable(after + ": stashed " + n + " is still there"));
                    }
                    const Result back = applyStash(repo, 0, true);
                    QVERIFY2(back.ok(), qPrintable(after + ": " + back.error()));
                    QVERIFY(stashes(repo).isEmpty());
                    // (What was staged is not, unless it was new: as git has it.)
                    for (auto& [n, x] : files) {
                        bool ok = false;
                        const QByteArray at = fileAt(repo, QStringLiteral(":0"), n, &ok);
                        x.index = ok ? std::optional<QByteArray>(at) : std::nullopt;
                    }
                    break;
                }
                case 12: {
                    // A line in .gitignore: that file's alone.
                    if (f.index || f.head || name.contains(QLatin1Char('\n')) || name.contains(QLatin1Char('\r'))) continue;
                    after = say("ignored for a moment " + name);
                    const QString ignoreFile = repo + "/.git/info/exclude";
                    const QByteArray kept = readAll(ignoreFile);
                    const QByteArray oldIgnore = readAll(repo + "/.gitignore");
                    const bool had = QFileInfo::exists(repo + "/.gitignore");
                    QString why;
                    QVERIFY2(ignore(repo, name, &why), qPrintable(after + ": " + why));
                    {
                        const QByteArray text = "x\n";
                        const bool wasThere = f.work.has_value();
                        if (!wasThere) write(repo + "/" + name, text);
                        const Repository r = read(repo);
                        const Entry* e = r.entryOf(name);
                        QVERIFY2(e != nullptr && e->ignored, qPrintable(after + ": " + (e == nullptr ? QStringLiteral("no entry") : e->describe())));
                        for (const auto& [n, x] : files) {
                            if (n == name || !x.work || x.index || x.head) continue;
                            const Entry* other = r.entryOf(n);
                            QVERIFY2(other != nullptr && !other->ignored, qPrintable(after + ": the line takes " + n + " too"));
                        }
                        if (!wasThere) QFile::remove(repo + "/" + name);
                    }
                    if (had) write(repo + "/.gitignore", oldIgnore);
                    else QFile::remove(repo + "/.gitignore");
                    Q_UNUSED(kept);
                    break;
                }
                }
                const QString disk = onDiskIsRight(after);
                QVERIFY2(disk.isEmpty(), qPrintable(disk));
                if (ops % 6 == 0) {
                    const QString said = saysRight(after);
                    QVERIFY2(said.isEmpty(), qPrintable(said));
                    ++checks;
                }
            }
        }
        qInfo().noquote() << QStringLiteral("repository: seed %1, %2 steps on %3 repositories, %4 full checks, in %5 s")
                                 .arg(p.seed).arg(step).arg(repositories).arg(checks).arg(clock.elapsed() / 1000);
    }
};

QTEST_MAIN(TestGitFuzz)
#include "test_git_fuzz.moc"
