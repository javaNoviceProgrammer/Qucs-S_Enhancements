#!/usr/bin/env python3
"""Each fix of the 7 November git pass broken in turn, and the test that should
catch it run: breaks.py <build dir> [name filter] (from the repository's top;
Release build). Each changes one place in the sources, builds that test, runs
that function alone, and puts the source back whatever happens. Stops nothing
on a break that escapes: it says so at the end.
"""
import os, subprocess, sys

Q = "qucs-s-26.1.1/qucs/"
R, F, P, I = "test_git_robust", "test_git_fuzz", "test_git_perf", "test_git_integration"
B = [
 ("literal paths", "gitrepo.cpp", 'for (const QString& p : paths) out << (p == QLatin1String(".") ? p : QStringLiteral(":(literal)") + p);', 'out = paths;', R, "aFileNameIsNoPattern"),
 ("literal paths, conflicts", "gitrepo.cpp", 'for (const QString& p : paths) out << (p == QLatin1String(".") ? p : QStringLiteral(":(literal)") + p);', 'out = paths;', R, "aConflictsNameIsNoPattern"),
 ("looking: literal", "gitstatus.cpp", 'env.insert(QStringLiteral("GIT_LITERAL_PATHSPECS"), QStringLiteral("1"));', '', R, "aFileNameIsNoPattern"),
 ("a name beginning with ':' is relative", "gitrepo.cpp", 'return !p.startsWith(QLatin1Char(\':\')) && QFileInfo(p).isAbsolute();', 'return QFileInfo(p).isAbsolute();', R, "aFileNameIsNoPattern"),
 ("fileIn by QDir", "gitstatus.cpp", 'return root.endsWith(QLatin1Char(\'/\')) ? root + path : root + QLatin1Char(\'/\') + path;', 'return QDir(root).filePath(path);', R, "aFileNameIsNoPattern"),
 ("a timed-out git killed", "gitrepo.cpp", 'p.terminate();   // (Windows\' would only ask a window to close)\n    if (p.waitForFinished(kAskedToStopMs)) return;', '', R, "aStoppedCommitLeavesNoLock"),
 ("a cancelled job killed", "gitrepo.cpp", 'a_process->terminate();\n    QTimer::singleShot(kAskedToStopMs, this, [this] {\n        if (isRunning()) a_process->kill();\n    });', 'a_process->kill();', R, "aStoppedCommitLeavesNoLock"),
 ("a cancelled clone killed", "gitrepo.cpp", 'a_process->terminate();\n    QTimer::singleShot(kAskedToStopMs, this, [this] {\n        if (isRunning()) a_process->kill();\n    });', 'a_process->kill();', R, "aStoppedCloneLeavesNoFolder"),
 ("a job that cannot start", "gitrepo.cpp", 'if (a_process == nullptr) return;\n    if (!a_input.isEmpty())', 'if (!a_input.isEmpty())', R, "aJobThatCannotRun"),
 ("an untracked link read", "gitstatus.cpp", 'if (info.isSymLink()) {\n        f.added = 1;', 'if (false) {\n        f.added = 1;', R, "aLinkIsNotFollowed"),
 ("an untracked link shown", "gitstatus.cpp", 'if (info.isSymLink()) {\n            text +=', 'if (false) {\n            text +=', R, "aLinkIsNotFollowed"),
 ("a pipe's conflict marks", "gitrepo.cpp", 'if (!info.isFile() || info.size() > (qint64(64) << 20)) return false;', 'if (info.size() > (qint64(64) << 20)) return false;', R, "aLinkToAPipeOrADeviceIsNotRead"),
 ("signatures checked", "gitstatus.cpp", 'QStringLiteral("log.showSignature=false"), ', '', R, "signaturesAreNotChecked"),
 ("a filter named a=b", "gitstatus.cpp", 'if (g.first(at).contains(QLatin1Char(\'=\'))) addConfig(env, g.left(at), g.mid(at + 1));\n        else args', 'args', R, "aFilterNamedWithAnEqualsSign"),
 ("a submodule entered", "gitrepo.cpp", 'QStringLiteral("--untracked-files=normal"), QStringLiteral("--ignore-submodules=dirty")};', 'QStringLiteral("--untracked-files=normal")};', R, "aSubmodulesOwnProgramsAreNotRun"),
 ("lazy fetch", "gitstatus.cpp", 'env.insert(QStringLiteral("GIT_NO_LAZY_FETCH"), QStringLiteral("1"));', '', R, "aPartialClonesFetchIsNotRun"),
 ("a hash is any text", "gitrepo.cpp", 'if (!isHash(c.hash)) continue;', '', F, "fuzzTheReaders"),
 ("a .git folder with no HEAD", "gitrepo.cpp", '|| (QFileInfo::exists(dotGit.filePath() + QStringLiteral("/HEAD")) && QFileInfo::exists(dotGit.filePath() + QStringLiteral("/objects"))\n                    && QFileInfo::exists(dotGit.filePath() + QStringLiteral("/refs")))))', '|| true))', R, "aFolderCalledDotGitIsNoRepository"),
 ("am as a rebase", "gitrepo.cpp", 'if (git.exists(QStringLiteral("rebase-apply/applying"))) r.operation = QStringLiteral("am");\n    else if', 'if', R, "whatIsUnderWayIsSaidRightly"),
 ("am not aborted", "gitrepo.cpp", 'operation == QLatin1String("rebase") || operation == QLatin1String("am")', 'operation == QLatin1String("rebase")', R, "whatIsUnderWayIsSaidRightly"),
 ("a merge first", "gitrepo.cpp", 'if (git.exists(QStringLiteral("rebase-apply/applying"))) r.operation', 'if (git.exists(QStringLiteral("MERGE_HEAD"))) r.operation = QStringLiteral("merge");\n    else if (git.exists(QStringLiteral("rebase-apply/applying"))) r.operation', R, "whatIsUnderWayIsSaidRightly"),
 ("names quoted", "gitrepo.cpp", 'QStringLiteral("--exclude-standard"), QStringLiteral("-z")};\n        if (!rel.isEmpty()) others', 'QStringLiteral("--exclude-standard")};\n        if (!rel.isEmpty()) others', R, "untrackedFilesWithAnyNameAreShown"),
 ("a tab in a name's counts", "gitrepo.cpp", 'QString path = c.at(i).mid(tab2 + 1);', 'QString path = c.at(i).mid(tab2 + 1).section(QLatin1Char(\'\\t\'), 0, 0);', R, "untrackedFilesWithAnyNameAreShown"),
 ("accents: one spelling", "gitrepo.cpp", 'return path.normalized(QString::NormalizationForm_C);', 'return path;', R, "anAccentIsTheSameInEitherForm"),
 ("a partial clone's remote", "gitrepo.cpp", '\\\\((fetch|push)\\\\)( \\\\[.*\\\\])?$', '\\\\((fetch|push)\\\\)$', R, "remotesOfAPartialClone"),
 ("colours", "gitstatus.cpp", 'addConfig(env, QStringLiteral("color.ui"), QStringLiteral("false"));', '', R, "colourSettingsLeaveTheTextsPlain"),
 ("a branch named as an option", "gitrepo.cpp", 'if (optionLike(repo.branch)) {', 'if (false) {', R, "aNameFromTheRepositoryIsNoOption"),
 ("a remote named as an option", "gitrepo.cpp", 'if (optionLike(remote)) {\n        if (why', 'if (false) {\n        if (why', R, "aNameFromTheRepositoryIsNoOption"),
 ("gh's option", "gitstatus.cpp", ' || branch.startsWith(QLatin1Char(\'-\'))) return pr;', ') return pr;', R, "aNameFromTheRepositoryIsNoOption"),
 ("a line break in .gitignore", "gitrepo.cpp", 'if (rel.contains(QLatin1Char(\'\\n\')) || rel.contains(QLatin1Char(\'\\r\'))) {', 'if (false) {', R, "aLineInGitignoreIsForTheFileAlone"),
 ("trailing spaces in .gitignore", "gitrepo.cpp", '.contains(c) || i >= textEnd) pattern', '.contains(c)) pattern', R, "aLineInGitignoreIsForTheFileAlone"),
 ("out of the repository in .gitignore", "gitrepo.cpp", ' || rel == QLatin1String("..")\n        || rel.startsWith(QLatin1String("../"))) {', ') {', R, "aLineInGitignoreIsForTheFileAlone"),
 ("both versions as text", "gitui.cpp", 'const QByteArray bytes = fileAt(root, stage, path);', 'const QByteArray bytes = QString::fromUtf8(fileAt(root, stage, path)).toUtf8();', R, "bothVersionsAreWrittenByteForByte"),
 ("an old read kept", "gitrepo.cpp", 'if (a_readDirect.value(root) < turn) store(root, watcher->result());', 'store(root, watcher->result());', R, "anOldReadDoesNotReplaceANewOne"),
 ("discard in one spelling", "gitrepo.cpp", 'p = lookupForm(p);\n            const QString path = lookupForm(e.path);', 'const QString path = e.path;', R, "anAccentIsTheSameInEitherForm"),
 ("unstage before the first commit", "gitrepo.cpp", 'QStringLiteral("-r"), QStringLiteral("-q"), QStringLiteral("-f")}, rel));', 'QStringLiteral("-r"), QStringLiteral("-q")}, rel));', R, "aRepositoryInEveryState"),
 ("a staged deletion staged again", "gitrepo.cpp", 'if (info.exists() || info.isSymLink() || p == QLatin1String(".")) {', 'if (true) {', R, "aRepositoryInEveryState"),
 ("HEAD with no commit", "gitrepo.cpp", 'QStringLiteral("--ignore-missing"), ', '', R, "aRepositoryInEveryState"),
 ("a new file's end of line", "gitrepo.cpp", 'if (!lastHasEnd) text += QStringLiteral("\\\\ No newline at end of file\\n");', '', R, "aNewFilesDiffIsAsGitWritesIt"),
 ("a new file's mode", "gitrepo.cpp", 'info.isExecutable() ? QStringLiteral("100755") : QStringLiteral("100644")', 'QStringLiteral("100644")', R, "aNewFilesDiffIsAsGitWritesIt"),
 ("more new files than shown", "gitrepo.cpp", 'if (untracked.size() > kNewFilesShown) text +=', 'if (false) text +=', R, "aNewFilesDiffIsAsGitWritesIt"),
 ("schematics by plain lines", "schematicdiff.cpp", 'for (const ChangedFile& f : changedFiles(root, commit)) {', 'QList<ChangedFile> plain;\n    for (const QString& l : run(root, {QStringLiteral("diff-tree"), QStringLiteral("--no-commit-id"), QStringLiteral("--name-status"), QStringLiteral("-r"), QStringLiteral("-M"), QStringLiteral("--root"), commit}, true, 30000).out.split(QLatin1Char(\'\\n\'), Qt::SkipEmptyParts)) {\n        ChangedFile f;\n        f.status = l.at(0);\n        f.path = l.section(QLatin1Char(\'\\t\'), 1, 1);\n        plain << f;\n    }\n    for (const ChangedFile& f : plain) {', R, "aSchematicOfAnyNameAndAMergesAreSummed"),
 ("a read that asks for its folder", "gitrepo.cpp", 'if (!dotGit.isSymLink()) {\n        if (dotGit.isDir())', 'if (false) {\n        if (dotGit.isDir())', P, "theProgramsEachStarts"),
 ("guards not kept", "gitstatus.cpp", 'if (it != kept.cend() && clock.elapsed() - it->at < 30000) return it->guards;', '', P, "theProgramsEachStarts"),
 ("a commit's files not kept", "gitrepo.cpp", 'const bool lasting = isHash(commit);', 'const bool lasting = false;', P, "theProgramsEachStarts"),
 ("a batch of blobs a git each", "gitrepo.cpp", 'if (!specs.at(i).contains(QLatin1Char(\'\\n\'))) {\n            input +=', 'if (false) {\n            input +=', P, "theProgramsEachStarts"),
 ("a diff's whole status", "gitrepo.cpp", 'const QStringList rel = path.isEmpty() ? QStringList() : relative(root, {path});\n    if (!path.isEmpty() && rel.isEmpty()) return {};\n    // The untracked files', '(void)read(root);\n    const QStringList rel = path.isEmpty() ? QStringList() : relative(root, {path});\n    if (!path.isEmpty() && rel.isEmpty()) return {};\n    // The untracked files', P, "theProgramsEachStarts"),
 ("a menu's status", "githistory.cpp", 'const Repository* looked = Tracker::instance()->fresh(root, 5000);', 'const Repository readNowInstead = read(root);\n    const Repository* looked = &readNowInstead;', P, "theProgramsEachStarts"),
 ("a file followed no more, discarded", "gitrepo.cpp", 'for (const QString& p : std::as_const(restore)) toTrash.removeAll(p);', '', R, "aFileNoLongerFollowedIsDiscardedBack"),
 ("Claude's commit of '.'", "qucscontrol_git.cpp", 'commitArgs << git::literalPaths({rel.isEmpty() ? QStringLiteral(".") : rel});', 'commitArgs << rel;', I, "claudesGitTools"),
 ("Claude's commit by patterns", "qucscontrol_git.cpp", 'commitArgs << git::literalPaths({rel.isEmpty() ? QStringLiteral(".") : rel});', 'commitArgs << (rel.isEmpty() ? QStringLiteral(".") : rel);', I, "claudesGitTools"),
 ("':' to Qt a resource, Claude's paths", "qucscontrol_git.cpp", 'const bool isRelative = p.startsWith(QLatin1Char(\':\')) || QFileInfo(p).isRelative();', 'const bool isRelative = QFileInfo(p).isRelative();', I, "claudesGitTools"),
]
build = sys.argv[1]
only = sys.argv[2] if len(sys.argv) > 2 else ""
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
caught, escaped, bad = 0, [], []
for name, file, old, new, target, fn in B:
    if only not in name or fn == "__none__":
        continue
    path = Q + file
    src = open(path).read()
    if src.count(old) != 1:
        bad.append(f"{name}: {src.count(old)} places")
        print("NO PLACE", name, flush=True)
        continue
    try:
        open(path, "w").write(src.replace(old, new))
        b = subprocess.run(["cmake", "--build", build, "--target", target, "--parallel"], capture_output=True, text=True)
        if b.returncode != 0:
            bad.append(f"{name}: does not build: " + b.stdout[-300:])
            print("NO BUILD", name, flush=True)
            continue
        try:
            r = subprocess.run([f"{build}/qucs/tests/{target}", fn], capture_output=True, text=True, env=env, timeout=240)
            failed = r.returncode != 0
        except subprocess.TimeoutExpired:
            failed = True
        if failed:
            caught += 1
        else:
            escaped.append(name)
        print("caught " if failed else "ESCAPED", name, f"({fn})", flush=True)
    finally:
        open(path, "w").write(src)
print(f"{caught} breaks caught, {len(escaped)} escaped {escaped}, {len(bad)} not made {bad}", flush=True)
