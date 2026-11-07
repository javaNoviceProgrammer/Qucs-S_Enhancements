#!/usr/bin/env python3
"""Each fix of the 7 November git pass broken in turn, and the test that should
catch it run: breaks.py <build dir> [name filter] (from the repository's top;
Release build). Each changes one place in the sources, builds that test, runs
that function alone, and puts the source back whatever happens. Stops nothing
on a break that escapes: it says so at the end.
"""
import os, subprocess, sys

Q = "qucs-s-26.1.1/qucs/"
R, F, P, I, PF = "test_git_robust", "test_git_fuzz", "test_git_perf", "test_git_integration", "test_project_folders"
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
 ("project: a pipe tested as a schematic", "schematic_file.cpp", 'if(!misc::isPlainFile(DocName) || QFileInfo(DocName).size() > kMostRead) return -1;', '', PF, "aPipeInAProjectStopsNothing"),
 ("project: a pipe's library record", "projectlibraries.cpp", 'if (!misc::isPlainFile(file) || !f.open(QIODevice::ReadOnly)) return false;', 'if (!f.open(QIODevice::ReadOnly)) return false;', PF, "aPipeInAProjectStopsNothing"),
 ("project: a pipe opened by name", "qucs.cpp", 'if (Info.exists() && !Info.isFile() && !Info.isDir()) {', 'if (false) {', PF, "aPipeInAProjectStopsNothing"),
 ("project: a subcircuit's pipe", "components/subcircuit.cpp", 'if (!misc::isPlainFile(file.fileName()) || !file.open(QIODevice::ReadOnly))\n', 'if (!file.open(QIODevice::ReadOnly))\n', PF, "aSchematicsPartsCanNamePipes"),
 ("project: a Verilog part's pipe", "components/verilogfile.cpp", '\n  if (!misc::isPlainFile(f.fileName()) || !f.open(QIODevice::ReadOnly))\n', '\n  if (!f.open(QIODevice::ReadOnly))\n', PF, "aSchematicsPartsCanNamePipes"),
 ("project: a VHDL part's pipe", "components/vhdlfile.cpp", '\n  if (!misc::isPlainFile(f.fileName()) || !f.open(QIODevice::ReadOnly))\n', '\n  if (!f.open(QIODevice::ReadOnly))\n', PF, "aSchematicsPartsCanNamePipes"),
 ("project: a text's .cfg pipe", "textdoc.cpp", 'if (!misc::isPlainFile (file.fileName ()) || !file.open (QIODevice::ReadOnly))', 'if (!file.open (QIODevice::ReadOnly))', PF, "aSchematicsPartsCanNamePipes"),
 ("project: a pipe read as a document", "schematic_file.cpp", 'if(!misc::isPlainFile(a_DocName) || !file.open(QIODevice::ReadOnly)) {', 'if(!file.open(QIODevice::ReadOnly)) {', PF, "aPipeInAProjectStopsNothing"),
 ("guards: kept whatever the config", "gitstatus.cpp", 'if (it != kept.cend() && it->stamp == stamp && clock.elapsed() - it->at < 30000) return it->guards;', 'if (it != kept.cend() && clock.elapsed() - it->at < 30000) return it->guards;', R, "aFilterAddedJustAfterALookIsGuarded"),
 ("guards: a worktree's own folder only", "gitstatus.cpp", 'if (!where.isEmpty()) common = QFileInfo(where).isAbsolute() ? where : fileIn(gitDir, where);', '', R, "aFilterAddedJustAfterALookIsGuarded"),
 ("guards: an include not noticed", "gitstatus.cpp", 'const bool lasting = none || (!files.isEmpty() && !includesOthers(files));', 'const bool lasting = true;', R, "aFilterAddedJustAfterALookIsGuarded"),
 ("review: a typo staged in silence", "gitrepo.cpp", 'if (!known.ok() || !known.out.trimmed().isEmpty() || !inHead(root, p)) some << p;', 'if (!known.ok() || !known.out.trimmed().isEmpty()) some << p;', R, "aRepositoryInEveryState"),
 ("review: a typo discarded in silence", "gitrepo.cpp", 'return failure(QStringLiteral("error: pathspec \'%1\' did not match any file(s) known to git").arg(p));', 'continue;', R, "aRepositoryInEveryState"),
 ("review: the prefix in git's words", "gitrepo.cpp", 'const QString err = QString(this->err).remove(QStringLiteral(":(literal)"));', 'const QString err = this->err;', R, "aRepositoryInEveryState"),
 ("review: show of a path it did not change", "gitrepo.cpp", 'if (limited && r.out.trimmed().isEmpty()) {', 'if (false) {', R, "aCommitIsShownWhateverThePath"),
 ("review2: a path outside taken as the top folder", "qucscontrol_git.cpp", 'if (const QString why = outsideOf(root, paths); !why.isEmpty()) {\n        done(errorResult(why));\n        return;\n    }\n', '', I, "aCommitWithAPathOutsideCommitsNothing"),
 ("review2: a list with a path outside staged", "qucscontrol_git.cpp", 'if (const QString why = all ? QString() : outsideOf(root, paths); !why.isEmpty()) return errorResult(why);\n    const git::Result r = git::stage(', 'const git::Result r = git::stage(', I, "aCommitWithAPathOutsideCommitsNothing"),
 ("review2: a pull with no setting stops", "gitrepo.cpp", 'args << QStringLiteral("--no-rebase");', '', I, "pullOfBranchesThatDiverged"),
 ("review2: a pull overrides a setting", "gitrepo.cpp", 'if (!isSet(QStringLiteral("pull.rebase")) && !isSet(QStringLiteral("pull.ff"))', 'if (true', R, "pullSaysHowOnlyWhereNothingDoes"),
 ("review2: a pull in conflict a plain error", "qucscontrol_git.cpp", 'if (action == QLatin1String("pull") && saysConflict(r)) {', 'if (false) {', I, "pullOfBranchesThatDiverged"),
 ("review2: a name's spaces trimmed", "qucscontrol_git.cpp", 'const QString p = v.toString();\n        if (p.trimmed().isEmpty()) continue;', 'const QString p = v.toString().trimmed();\n        if (p.isEmpty()) continue;', I, "aDiscardTakesTheFileNamed"),
 ("review2: only the last in an untracked folder", "gitrepo.cpp", 'lookupForm(p).startsWith(lookupForm(e.path))) targets << p;', 'lookupForm(p).startsWith(lookupForm(e.path))) targets = {p};', I, "aDiscardTakesTheFileNamed"),
 ("review2: a name in an untracked folder taken unseen", "gitrepo.cpp", 'if (!within || QFileInfo::exists(fileIn(root, given))) taken.insert(given);', 'taken.insert(given);', I, "aDiscardTakesTheFileNamed"),
 ("review2: a rename's old name unseen", "gitrepo.cpp", ' || within || (!e.from.isEmpty() && lookupForm(e.from) == p)) {', ' || within) {', I, "aDiscardTakesTheFileNamed"),
 ("review2: another case of a name", "gitrepo.cpp", 'for (QString& p : rel) p = spelledOnDisk(root, p);', '', I, "aDiscardTakesTheFileNamed"),
 ("review2: a side's alone restored from HEAD", "gitrepo.cpp", '\n                   || (e.conflicted && !inHead(root, e.path))) {', ') {', I, "discardAllInAMerge"),
 ("review2: a pipe named .gitignore", "gitrepo.cpp", 'if (ignoreFile.isSymLink() || (ignoreFile.exists() && !ignoreFile.isFile())) {', 'if (ignoreFile.isSymLink()) {', R, "aGitignoreThatIsNoPlainFile"),
 ("review2: written through a link", "gitrepo.cpp", 'if (ignoreFile.isSymLink() || (ignoreFile.exists() && !ignoreFile.isFile())) {', 'if (!ignoreFile.isSymLink() && ignoreFile.exists() && !ignoreFile.isFile()) {', R, "aGitignoreThatIsNoPlainFile"),
 ("review2: a link to a folder as a folder", "gitrepo.cpp", 'const bool folder = what.isDir() && !what.isSymLink();', 'const bool folder = what.isDir();', R, "aGitignoreThatIsNoPlainFile"),
 ("review2: no conflict called one", "qucscontrol_git.cpp", 'if (saysConflict(r) || after.conflictCount() > 0) {', 'if (saysConflict(r) || r.err.contains(QLatin1String("conflict"))) {', I, "whatIsNotAConflictIsNotCalledOne"),
 ("review2: a commit in a rebase", "qucscontrol_git.cpp", 'if (const QString under = git::read(root).operation; !amend && (under == QLatin1String("rebase") || under == QLatin1String("am"))) {', 'if (const QString under = git::read(root).operation; false) {', I, "aRebaseStoppedGoesOn"),
 ("review2: continue with conflicts left", "qucscontrol_git.cpp", 'if (!skip && before.conflictCount() > 0)', 'if (false)', I, "aRebaseStoppedGoesOn"),
 ("review2: no Continue in the menu", "gitui.cpp", '->setObjectName(QStringLiteral("gitContinue"));', ';', I, "aRebaseStoppedGoesOn"),
 ("review2: an empty side taken as deleted", "gitrepo.cpp", 'if (v.incomplete) return failure(', 'if (false) return failure(', R, "aSideNotReadIsNotASideThatDeleted"),
 ("review2: a typo unstaged in silence", "gitrepo.cpp", 'if (known.ok() && known.out.trimmed().isEmpty())\n                return failure(', 'if (false)\n                return failure(', I, "theSmallerFaultsOfTheReview"),
 ("review2: no such file as committed", "qucscontrol_git.cpp", 'e != nullptr ? e->describe() : there ? tr("as committed") : tr("no such file")', 'e != nullptr ? e->describe() : tr("as committed")', I, "theSmallerFaultsOfTheReview"),
 ("review2: an unknown ref an empty list", "qucscontrol_git.cpp", 'if (!ref.isEmpty() && !git::run(root, {QStringLiteral("rev-parse")', 'if (false && !git::run(root, {QStringLiteral("rev-parse")', I, "theSmallerFaultsOfTheReview"),
 ("review2: a stash that is not there", "qucscontrol_git.cpp", 'if (index >= git::stashes(root).size()) return errorResult(', 'if (false) return errorResult(', I, "theSmallerFaultsOfTheReview"),
 ("review2: a file named as the ref, log", "gitrepo.cpp", 'QStringList limit{QStringLiteral("--")};   // (a file named as the ref: the ref, not "both")', 'QStringList limit;', I, "theSmallerFaultsOfTheReview"),
 ("review2: a file named as the commit, show", "gitrepo.cpp", 'args << QStringLiteral("--");   // (a file named as the commit: the commit, not "both")\n    if (limited) args << rel;', 'if (limited) args << QStringLiteral("--") << rel;', I, "theSmallerFaultsOfTheReview"),
 ("review2: a file named as the commit, reset", "gitrepo.cpp", 'QStringLiteral("--") + mode, commit, QStringLiteral("--")});', 'QStringLiteral("--") + mode, commit});', I, "theSmallerFaultsOfTheReview"),
 ("review2: a stash's new files", "gitrepo.cpp", 'withNew.insert(3, QStringLiteral("--include-untracked"));', '', I, "theSmallerFaultsOfTheReview"),
 ("review2: a rename by its new name", "gitrepo.cpp", 'if (e.staged == QLatin1Char(\'R\') && !e.from.isEmpty() && paths.contains(e.path) && !out.contains(e.from)) out << e.from;', '(void)e;', I, "theSmallerFaultsOfTheReview"),
 ("review2: blame cut in silence", "qucscontrol_git.cpp", 'if (last < all.last().line) blamed.insert(', 'if (false) blamed.insert(', I, "theSmallerFaultsOfTheReview"),
 ("review2: unstage all of nothing", "gitrepo.cpp", 'QStringLiteral("-f"),\n                                    QStringLiteral("--ignore-unmatch")}, rel));', 'QStringLiteral("-f")}, rel));', I, "theSmallerFaultsOfTheReview"),
 ("review2: a clone's name from a slash", "gitrepo.cpp", "while (name.endsWith(QLatin1Char('/'))) name.chop(1);", '', R, "theSmallerNames"),
 ("review2: a remote's name with a slash", "gitrepo.cpp", 'if (r.name.size() > remoteName.size() && branch.startsWith(r.name + QLatin1Char(\'/\'))) remoteName = r.name;', '(void)r;', I, "theSmallerFaultsOfTheReview"),
 ("review2: git's messages translated", "gitstatus.cpp", 'env.insert(QStringLiteral("LANGUAGE"), QStringLiteral("C"));', '', R, "theSmallerNames"),
 ("review2: a fatal's lines cut", "gitrepo.cpp", '} else if (goesOn && !t.startsWith(QLatin1String("hint:"))) {', '} else if (false) {', R, "theSmallerNames"),
 ("a read that asks for its folder", "gitrepo.cpp", 'if (!dotGit.isSymLink()) {\n        if (dotGit.isDir())', 'if (false) {\n        if (dotGit.isDir())', P, "theProgramsEachStarts"),
 ("guards not kept", "gitstatus.cpp", 'if (it != kept.cend() && it->stamp == stamp && clock.elapsed() - it->at < 30000) return it->guards;', '', P, "theProgramsEachStarts"),
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
