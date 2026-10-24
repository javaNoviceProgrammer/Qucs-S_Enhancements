# The fixes of the A-class findings broken on purpose: each break of BREAKS is applied to
# the source, the tests it names are built in build/ and run - each must fail
# -, then the source is put back. Rebuild afterwards: the source is
# restored, the build is not.
#   HUNT_RUNS=<scratch> python3 breaks.py [name ...]
#   BRK_ONLY_CHECK=1 ...: each break's text looked for, nothing built
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
SRC = os.path.join(ROOT, 'qucs-s-26.1.1', 'qucs')
H = os.environ['HUNT_RUNS']
W = 'test_mcp_wishlist:'
ERC = 'test_erc:'
D = 'test_data_import:'
E = 'test_fem_engine:selectionRules'

BREAKS = [
    # ---- F1, A5, A6: the reload of a file half written
    ('F1 the box raised from the reload', 'qucs.cpp',
     '    misc::ErrorCapture quiet;\n    how = reloadFromDisk(doc);\n    why = quiet.errors().join(QStringLiteral("; "));',
     '    { misc::ErrorCapture quiet; (void)quiet.errors(); }\n    how = reloadFromDisk(doc);',
     [W + 'aHalfWrittenSchematicIsNotTakenForTheDocument'], {'timeout': 180}),
    ('A5 the revision not put back', 'schematic.cpp',
     '        if (held) rewind(wasRevision, wasEdits);   // nothing of it changed', '',
     [W + 'aHalfWrittenSchematicIsNotTakenForTheDocument'], {}),
    # ---- A1: only a real plural folded
    ('A1 a one-letter key folded', 'qucscontrol.cpp',
     '    if (word.size() < 3) return {};   // \'a\' for \'as\', \'v\' for \'vs\', \'f\' for \'fs\'',
     '    if (false) return {};',
     [W + 'onlyARealPluralIsReadFromItsSingular'], {}),
    ('A1 a flag folded', 'qucscontrol.cpp',
     '        if (type.toString() == QLatin1String("boolean")) continue;', '        if (false) continue;',
     [W + 'onlyARealPluralIsReadFromItsSingular'], {}),
    ('A1 the names that are no plurals folded', 'qucscontrol.cpp',
     '    return notPlurals.contains(field);', '    return false;',
     [W + 'onlyARealPluralIsReadFromItsSingular'], {}),
    # ---- A2, A3: the guards given what the tool reads
    ('A2 guarded() given the raw arguments', 'qucscontrol_resources.cpp',
     '    const QJsonObject a = asTheToolReadsIt(tool, given);\n    Guarded g;', '    const QJsonObject a = given;\n    Guarded g;',
     [W + 'theGuardsSeeWhatTheToolWillRead'], {}),
    ('A3 irreversible() given the raw arguments', 'qucscontrol_resources.cpp',
     '    const QJsonObject a = asTheToolReadsIt(tool, given);   // (as guarded(): A2, A3)', '    const QJsonObject a = given;',
     [W + 'theGuardsSeeWhatTheToolWillRead'], {}),
    ('A3 askedEachTime() given the raw arguments', 'qucscontrol_resources.cpp',
     '        const QJsonObject a = asTheToolReadsIt(tool, given);\n        // A batch: as the first of its calls that is.',
     '        const QJsonObject a = given;\n        // A batch: as the first of its calls that is.',
     [W + 'theGuardsSeeWhatTheToolWillRead'], {}),
    # ---- A4: a script's names
    ('A4 a name not written out let through', 'qucscontrol_resources.cpp',
     '        if (!aLiteral.match(script.mid(at.capturedEnd())).hasMatch()) return true;', '        if (false) return true;',
     [W + 'aScriptIsAskedAboutWhateverWayItNamesTheTool'], {}),
    ('A4 the old five words alone', 'qucscontrol_resources.cpp',
     '    for (const QString& tool : named)\n        if (script.contains(tool)) return true;',
     '    for (const QString& tool : {QStringLiteral("clean_scratch"), QStringLiteral("trash_file")})\n        if (script.contains(tool)) return true;',
     [W + 'aScriptIsAskedAboutWhateverWayItNamesTheTool'], {}),
    # ---- A7: no reload under a job
    ('A7 the job not looked for', 'qucs.cpp',
     '  if (auto *control = findChild<QucsControl *>(); control != nullptr && control->busyWithAJob(doc)) {',
     '  if (false) {',
     [W + 'aModelIsNotReadAgainUnderItsOwnJob'], {'timeout': 600}),
    ('A7 a model never busy', 'qucscontrol_watch.cpp',
     '    if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc)) return model->isBusy();',
     '    if (dynamic_cast<MultiphysicsDoc*>(doc) != nullptr) return false;',
     [W + 'aModelIsNotReadAgainUnderItsOwnJob'], {'timeout': 600}),
    # ---- A8, A13: the ways a file leaves the workspace
    ('A8 a rename not named', 'qucscontrol_resources.cpp',
     '    } else if (tool == QLatin1String("rename_file")) {', '    } else if (false) {',
     [W + 'everyWayAFileLeavesTheWorkspaceIsNamed'], {}),
    ('A13 a library not named', 'qucscontrol_resources.cpp',
     '    } else if (tool == QLatin1String("create_library")) {', '    } else if (false) {',
     [W + 'everyWayAFileLeavesTheWorkspaceIsNamed'], {}),
    ('A13 the setting not named', 'qucscontrol_resources.cpp',
     '    } else if (tool == QLatin1String("set_settings")) {', '    } else if (false) {',
     [W + 'everyWayAFileLeavesTheWorkspaceIsNamed'], {}),
    # ---- A9, A10, A11: the Touchstone reader
    ('A9 the points not counted', 'dataimport.cpp',
     '    if (!stopped && read != numbers.size())', '    if (false)',
     [D + 'aDamagedTouchstoneFileIsNotGuessedAt'], {}),
    ('A10 the dropped rows not said', 'dataimport.cpp',
     '    if (stopped && (numbers.size() - read) % 5 != 0)', '    if (false)',
     [D + 'aDamagedTouchstoneFileIsNotGuessedAt'], {}),
    ('A11 an unknown unit guessed', 'dataimport.cpp',
     '    if (optionSeen && !unitSaid)', '    if (false)',
     [D + 'aDamagedTouchstoneFileIsNotGuessedAt'], {}),
    ('A11 the missing option line not said', 'dataimport.cpp',
     '    if (!optionSeen)\n        data->notes << tr("It has no \'#\' option line', '    if (false)\n        data->notes << tr("It has no \'#\' option line',
     [D + 'aDamagedTouchstoneFileIsNotGuessedAt'], {}),
    # ---- A12: a missing file reported for every file part
    ('A12 the file parts not checked', 'erc.cpp',
     '        if (c->Model == QLatin1String("SPfile") || c->Model == QLatin1String("SPICE") || c->Model == QLatin1String("Ifile")',
     '        if (false && (c->Model == QLatin1String("SPfile") || c->Model == QLatin1String("SPICE") || c->Model == QLatin1String("Ifile"))',
     [ERC + 'everyPartBackedByAFileSaysWhenItIsMissing'], {}),
]


def watched(cmd, env, timeout, rss_kb):
    """The test run, killed past \a timeout seconds or \a rss_kb of memory: what stopped it, or its result."""
    import tempfile, time
    out, err = tempfile.TemporaryFile('w+'), tempfile.TemporaryFile('w+')
    p = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=out, stderr=err, text=True)
    start, why = time.time(), None
    while p.poll() is None:
        time.sleep(0.25)
        if time.time() - start > timeout:
            why = f'past {timeout} s'
        elif rss_kb:
            r = subprocess.run(['ps', '-o', 'rss=', '-p', str(p.pid)], capture_output=True, text=True).stdout.strip()
            if r.isdigit() and int(r) > rss_kb:
                why = f'past {rss_kb >> 20} GB of memory'
        if why:
            p.kill()
            p.wait()
            return why
    out.seek(0), err.seek(0)
    return subprocess.CompletedProcess(cmd, p.returncode, out.read(), err.read())


def run(name, path, old, new, tests, opts):
    # (path, old, new), a list of them, or old ('before', start, end): that part as it was at BEFORE.
    if isinstance(path, list):
        edits = path
    elif isinstance(old, tuple):
        now, was = before(path, old[1], old[2])
        edits = [(path, now, was)]
    else:
        edits = [(path, old, new)]
    kept = {}
    for p, o, n in edits:
        src = os.path.join(SRC, p)
        kept.setdefault(src, open(src, encoding='utf-8').read())
        now = open(src, encoding='utf-8').read()
        if now.count(o) != 1 or o == n:
            for f, t in kept.items():
                open(f, 'w', encoding='utf-8').write(t)
            return f'{name}: its text found {now.count(o)} times in {p}' + (' (and is as it was)' if o == n else '')
        open(src, 'w', encoding='utf-8').write(now.replace(o, n))
    if os.environ.get('BRK_ONLY_CHECK'):
        for f, t in kept.items():
            open(f, 'w', encoding='utf-8').write(t)
        return f'{name}: found'
    build = os.path.join(ROOT, opts.get('build', 'build'))
    try:
        targets = sorted({t.split(':')[0] for t in tests})
        b = subprocess.run(['ninja', '-C', build] + [f'qucs/tests/{t}' for t in targets], capture_output=True, text=True)
        if b.returncode != 0:
            return f'{name}: the build failed\n{b.stdout[-1500:]}'
        env = dict(os.environ, HOME=H + '/home', QUCS_SETTINGS_DIR=H + '/settings', QUCS_TRASH_DIR=H + '/trash',
                   QUCS_CACHE_DIR=H + '/cache', QT_QPA_PLATFORM='offscreen',
                   ASAN_OPTIONS='detect_leaks=0' + (':' + opts['asan'] if 'asan' in opts else ''),
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        env.pop('QUCS_TEST_SLOW', None)
        caught = []
        for t in tests:
            exe, fn = t.split(':')
            p = watched([os.path.join(build, 'qucs', 'tests', exe), fn], env, opts.get('timeout', 900), opts.get('rss'))
            if isinstance(p, str):
                caught.append(f'{exe}: {p}')
                continue
            fails = [l for l in p.stdout.splitlines() if l.startswith('FAIL!')]
            report = [l for l in (p.stdout + p.stderr).splitlines() if 'runtime error' in l or 'ERROR: AddressSanitizer' in l]
            if p.returncode != 0 or fails:
                caught.append(f'{exe}: ' + (fails[0][:200] if fails else (report[0][:200] if report else f'exit {p.returncode}')))
        return f'{name}: ' + ('CAUGHT - ' + ' | '.join(caught) if caught else 'NOT CAUGHT')
    finally:
        for f, t in kept.items():
            open(f, 'w', encoding='utf-8').write(t)


for d in ('home', 'settings', 'trash', 'cache'):
    os.makedirs(os.path.join(H, d), exist_ok=True)
for name, path, old, new, tests, opts in BREAKS:
    if len(sys.argv) > 1 and not any(a in name for a in sys.argv[1:]):
        continue
    print(run(name, path, old, new, tests, opts), flush=True)
