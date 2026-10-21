# The fixes broken on purpose: each break of BREAKS is applied to the source,
# the tests it names are built in its tree (the Release tree, build/, unless
# it says build-asan) and run - each must fail, crash or pass its time -,
# then the source is put back. Rebuild both trees afterwards: the source is
# restored, the builds are not. The breaks that would take memory without
# end run under the sanitizers with a ceiling (hard_rss_limit_mb).
#   HUNT_RUNS=<scratch> python3 breaks.py [name ...]
#   BRK_ONLY_CHECK=1 ...: each break's text looked for, nothing built
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
SRC = os.path.join(ROOT, 'qucs-s-26.1.1', 'qucs')
H = os.environ['HUNT_RUNS']
# The commit before the fixes: two breaks put a whole function back as it was there.
BEFORE = '8c36b43'
ASAN = {'build': 'build-asan'}
# (hard_rss_limit_mb does nothing on macOS: a test past 3 GB is stopped here.)
CAPPED = {'build': 'build-asan', 'rss': 3 << 20}
E = 'test_fem_engine:hostileExpressions'
LAY = 'test_layout_doc:'


def before(path, start, end):
    """The text of \\a path from \\a start to \\a end, now and at BEFORE: a break that puts it back."""
    rel = os.path.relpath(os.path.join(SRC, path), ROOT)
    old = subprocess.run(['git', '-C', ROOT, 'show', f'{BEFORE}:{rel}'], capture_output=True, text=True, check=True).stdout
    now = open(os.path.join(SRC, path), encoding='utf-8').read()
    cut = lambda t: t[t.index(start):t.index(end, t.index(start))]
    return cut(now), cut(old)


BREAKS = [
    # Expressions (E1, E2, E4, E5; the compiler's depth, N1).
    ('nesting unbounded (E1)', 'multiphysics/fem_expr.cpp',
     '        if (a_nesting > MaxNesting) return fail(tr("the expression nests too deeply (more than %1 levels)").arg(MaxNesting));\n', '', [E], {}),
    ('height unbounded (E1)', 'multiphysics/fem_expr.cpp',
     '        if (height > MaxHeight) return fail(tr("the expression is too deep (more than %1 levels)").arg(MaxHeight));\n', '', [E], {}),
    ('functions written out unbounded (E2)', 'multiphysics/fem_expr.cpp',
     '        if (++a_nodes > MaxNodes) return failed(tr("the expression grows too large with its functions written out (more than %1 terms)").arg(MaxNodes));\n',
     '', [E], {}),
    ('a table of two at no number (E4, build-asan)', 'multiphysics/fem_expr.cpp',
     ' || std::isnan(u) || std::isnan(v)) return std::nan("");', ') return std::nan("");', [E], ASAN),
    ('a table of one at no number (E4, build-asan)', 'multiphysics/fem_expr.cpp',
     '    if (x.empty() || y.size() < x.size() || std::isnan(v)) return std::nan("");', '    if (x.empty()) return std::nan("");', [E], ASAN),
    ('powers wrapped (E5)', 'multiphysics/fem_expr.cpp',
     '    return std::int8_t(std::clamp(power, -127LL, 127LL));', '    return std::int8_t(power);', [E], {}),
    ("a unit's power unbounded (E5)", 'multiphysics/fem_expr.cpp', '            if (!ok || std::abs(n) >= 64) {', '            if (false) {', [E], {}),
    ('the compiler 64 deep (N1)', 'multiphysics/fem_expr.cpp', '        if (depth > MaxDepth) return failed(', '        if (depth > 64) return failed(', [E], {}),
    ("the functions' calls not counted (N1)", 'multiphysics/fem_expr.cpp',
     '            if (a_calls > MaxCalls) return failed(tr("the functions call each other too deeply"));\n', '', [E], {}),
    # Threads (E3).
    ("the pools' threads of 512 kB (E3, build-asan)", 'multiphysics/fem_solver.cpp', '    t->setStackSize(8u << 20);\n', '',
     ['test_fem_physics:aSweepsPointsAtOnceCompileDeepExpressions'], ASAN),
    # Layouts (L1, L2, L3).
    ('searched to any depth (L1)', 'layout.cpp', '        if (level < std::min(query.depth, MaxLevels)) {', '        if (level < query.depth) {',
     [LAY + 'aHierarchyThousandsDeepIsReadAndDrawn'], {}),
    ('drawn to any depth (L1, build-asan)', 'layout.cpp', '                if (level + 1 > std::min(O.depth, MaxLevels)) {', '                if (level + 1 > O.depth) {',
     [LAY + 'aHierarchyThousandsDeepIsReadAndDrawn'], ASAN),
    ('bounds by recursion (L1, build-asan)', 'layout.cpp', ('before', '// The bounds of every cell, bottom up;', '\n    for (Cell& cell : layout.cells) {\n        QSet<int> placed;'),
     None, [LAY + 'aHierarchyThousandsDeepIsReadAndDrawn'], ASAN),
    ("a text's properties copied before their names (L2, build-asan)", '../third_party/gdstk/src/library.cpp',
     ('before', '    auto resolve = [&]() {', '    OasisRecord record;'), None, [LAY + 'anOasisTextsPropertyNamedByNumberIsRead'], ASAN),
    ("a property's count unchecked (L3, build-asan, capped)", '../third_party/gdstk/src/library.cpp',
     '                        if (!oasis_count_fits(in, num_values)) {\n                            num_values = 0;',
     '                        if (false) {\n                            num_values = 0;', [LAY + 'anOasisPropertysCountPastTheFileIsSaidDamaged'], CAPPED),
    # Geometry (G1, G2).
    ("a vertex's cell rounded unclamped (G1, build-asan)", 'multiphysics/fem_geometry.cpp',
     '            return std::isfinite(c) ? static_cast<long long>(std::clamp(c, -4e18, 4e18)) : 0LL;', '            return std::llround(c);',
     ['test_fem_engine:hostilePositions'], ASAN),
    ("a grid's cell cast, then clamped (M1, build-asan)", 'multiphysics/fem_mesh.cpp',
     '        auto index = [this](double v) { return v >= 0 ? int(std::min(v, double(a_n - 1))) : 0; };',
     '        auto index = [this](double v) { return std::clamp(int(v), 0, a_n - 1); };', ['test_fem_engine:hostilePositions'], ASAN),
    ("the union's lost objects not said (G2)", 'multiphysics/fem_geometry.cpp',
     'lostText.isEmpty() ? QString::fromLocal8Bit(e.what()) : lostText', 'QString::fromLocal8Bit(e.what())', ['test_fem_engine:hostilePositions'], {}),
    # Mode profiles (P1).
    ('a character past 16 bits cast (P1)', 'multiphysics/fem_modeprofile.cpp',
     '            else if (c > 0xffff && c <= 0x10ffff) s += QStringView(QChar::fromUcs4(char32_t(c)));\n            else s += QChar(QChar::ReplacementCharacter);',
     '            else s += QChar(char16_t(c));', ['test_fem_modeoverlap:aUtf32AboutIsRead'], {}),
    # Process flows (PF1, PF2).
    ('a grid of no rows refined (PF1, build-asan)', 'multiphysics/fem_process_dopants.cpp',
     ('before', 'void Dopants::refine(const std::vector<Region>& regions)', '    const double want = '), None, ['test_fem_process:hostileFlows'], ASAN),
    ("the dopants' grid unbudgeted (PF2)", [
        ('multiphysics/fem_process_dopants.cpp', '    if (!fits(nx, (std::ceil(top / dy) * dy + dy - fineFrom) / dy)) return;\n', ''),
        ('multiphysics/fem_process_dopants.cpp', '    if (!fits(d.nx(), d.ny() + (top - d.ye.back()) / a_dy + 1)) return;\n', ''),
        ('multiphysics/fem_process_dopants.cpp', '    if (!fits(d.nx(), d.ny() + (fineFrom - from) / a_dy)) return;\n', '')], None, None,
     ['test_fem_process:hostileFlows'], {'timeout': 300}),
    # Counts read from a model (X1).
    ("a trap distribution's levels unbounded (X1, build-asan, capped)", 'multiphysics/fem_semiconductor.cpp',
     '                else if (count < 1 || count > 1000) problem(tr("%1: its levels from 1 to 1000, not %2").arg(where).arg(count));\n', '',
     ['test_fem_devices:trapLevelsAreBounded'], CAPPED),
    ("a scan's points unbounded (X1, build-asan, capped)", 'multiphysics/fem_waves.cpp', ' || pc.points > 1000)', ')',
     ['test_fem_waves:aScansPointsAreBounded'], CAPPED),
    # The tools (N2).
    ("a flow's materials changed, not built again (N5)", 'multiphysicsdoc.cpp', ' || (b.flow && a.materials != b.materials);', ';',
     ['test_multiphysics_doc:aBuiltGeometryIsNotBuiltAgain'], {}),
    ('fem_build builds again (N2)', 'qucscontrol_fem.cpp', '    if (!doc->currentGeometry(&why)) return errorResult(', '    if (!doc->buildGeometry(&why)) return errorResult(',
     ['test_multiphysics_doc:aBuiltGeometryIsNotBuiltAgain'], {}),
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
