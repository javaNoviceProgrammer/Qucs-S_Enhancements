# The fixes of the B-class findings broken on purpose: each break of BREAKS is applied to
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
    # ---- B1: a reference named for something no value answers to
    ('B1 the unnamed fallback let in a named one', 'qucscontrol_fem.cpp',
     'values.size() == 1 && reference.constBegin().key().isEmpty())', 'values.size() == 1)',
     [W + 'theToolsSayWhatTheyDidWithAnOddArgument'], {}),
    # ---- B2: a region that meets no text
    ('B2 a narrowed page called scanned', 'qucscontrol_design.cpp',
     'if (empty && corners.isEmpty())', 'if (empty)',
     [W + 'theToolsSayWhatTheyDidWithAnOddArgument'], {}),
    # ---- B3: a size that is no size
    ('B3 a size beyond any geometry taken', 'qucscontrol_fem.cpp',
     'std::abs(e.constant()) > 1e12', 'std::abs(e.constant()) > 1e400',
     [W + 'theToolsSayWhatTheyDidWithAnOddArgument'], {}),
    ('B3 an expression that does not read not said', 'qucscontrol_fem.cpp',
     '                        if (!e.isValid())\n                            checked << tr("%1: %2 does not read as an expression here (%3)")',
     '                        if (false)\n                            checked << tr("%1: %2 does not read as an expression here (%3)")',
     [W + 'theToolsSayWhatTheyDidWithAnOddArgument'], {}),
    # ---- B4: a plot group shown again
    ('B4 the display marked changed anyway', 'qucs_python.cpp',
     '      if (wrote) {', '      if (true) {',
     [W + 'aPlotGroupShownAgainLeavesItsDisplayAlone'], {'timeout': 600}),
    # ---- B5: a count of nought, in three tools
    ('B5 find_shapes clamps its limit', 'qucscontrol_layout.cpp',
     'if (args.contains(QLatin1String("limit")) && !(args.value(QLatin1String("limit")).toInt(0) >= 1',
     'if (false && !(args.value(QLatin1String("limit")).toInt(0) >= 1',
     [W + 'theToolsSayWhatTheyDidWithAnOddArgument'], {}),
    ('B5 fem_converge clamps its timeout', 'qucscontrol_fem.cpp',
     'if (args.contains(QLatin1String("timeout")) && args.value(QLatin1String("timeout")).toInt(-1) < 1)',
     'if (false)',
     [W + 'theToolsSayWhatTheyDidWithAnOddArgument'], {}),
    ('B5 tune clamps its max_runs', 'qucscontrol_design.cpp',
     'tr("\'max_runs\' is how many simulations at most, 2 to 40 (12 when not given): %1 is none of them.")',
     'tr("")',
     [W + 'theToolsSayWhatTheyDidWithAnOddArgument'], {}),
    # ---- B6: line_calc answered nan
    ('B6 the permittivity not checked', 'qucscontrol_synthesis.cpp',
     '                                    {"er", tr("the substrate\'s relative permittivity")},\n', '',
     [W + 'theNumericToolsRefuseWhatCannotBe'], {}),
    ('B6 no bound at all on an input', 'qucscontrol_synthesis.cpp',
     '    if (std::isfinite(x) && x > least) return {};', '    return {};',
     [W + 'theNumericToolsRefuseWhatCannotBe'], {}),
    ('B6 a result that is no number handed back', 'qucscontrol_synthesis.cpp',
     'if (const QString key = noNumberIn(r); !key.isEmpty())', 'if (const QString key = noNumberIn(r); false)',
     [W + 'theNumericToolsRefuseWhatCannotBe'], {}),
    # ---- B7: receiver_budget took the impossible
    ('B7 absolute zero not minded', 'qucscontrol_synthesis.cpp',
     'if (const QString why = mustBeAbove(args, "temperature", -273.15, tr("a temperature in C")); !why.isEmpty())',
     'if (const QString why = QString(); !why.isEmpty())',
     [W + 'theNumericToolsRefuseWhatCannotBe'], {}),
    ('B7 no two-sided bound', 'qucscontrol_synthesis.cpp',
     '    if (std::isfinite(x) && x >= least && x <= most) return {};', '    return {};',
     [W + 'theNumericToolsRefuseWhatCannotBe'], {}),
    # ---- B8: an empty action name
    ('B8 nothing names the first action', [
        ('qucscontrol.cpp', '    if (wanted.trimmed().isEmpty()) return nullptr;', '    if (false) return nullptr;'),
        ('qucscontrol.cpp', '(!actions.at(i)->objectName().isEmpty() && actions.at(i)->objectName() == wanted)',
         '(actions.at(i)->objectName() == wanted)')],
     None, None, [W + 'theToolsSayWhatTheyDidWithAnOddArgument'], {}),
    # ---- B9, B10: what a CSV's header says
    ('B9 the numbers begin at the widest row', 'dataimport.cpp',
     '    const qsizetype least = std::max<qsizetype>(std::min<qsizetype>(2, width), width - 1);',
     '    const qsizetype least = width;',
     [D + 'aCsvKeepsItsHeaderAndSaysWhatItRenamed'], {}),
    ('B9 the widths not said', 'dataimport.cpp',
     '    if (widths.size() > 1) {', '    if (false) {',
     [D + 'aCsvKeepsItsHeaderAndSaysWhatItRenamed'], {}),
    ('B10 the renames keyed by the old name', 'dataimport.cpp',
     '        if (names.size() == 1) {', '        if (true) {',
     [D + 'aCsvKeepsItsHeaderAndSaysWhatItRenamed'], {}),
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
