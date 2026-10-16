# The fixes broken on purpose: each break of BREAKS is applied to the source,
# the tests it names are built in BUILD (the Release tree, build/, unless
# BRK_BUILD names another) and run - each must fail or crash -, then the
# source is put back. Rebuild afterwards: the source is restored, the build
# is not. Two need the ASan/UBSan tree (an out-of-range cast is seen there
# alone): BRK_BUILD=build-asan python3 breaks.py "whole is a cast" ...
#   HUNT_RUNS=<scratch> python3 breaks.py [name ...]
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
SRC = os.path.join(ROOT, 'qucs-s-26.1.1', 'qucs')
BUILD = os.path.join(ROOT, os.environ.get('BRK_BUILD', 'build'))
H = os.environ['HUNT_RUNS']
W = 'test_fem_waves:searchesStopWhenAsked'
R = 'test_fem_resonator:stopsWhenAsked'
L = 'test_fem_lumerical:damagedInputsAreRefused'
BREAKS = [
    ('winding core read through (F1)', 'magnetics/winding.cpp',
     '      if (probes != nullptr) {\n        QString BH = probes->Value;',
     '      if (pc->Name == CORE) {\n        QString BH = pc->getProperty("BHprobes")->Value;', ['test_erc:aWindingsCoreThatIsNoCore']),
    ('every image scaled whole (F2)', 'markdowndoc.cpp', '    if (double(target.width()) * target.height() > 16e6) {',
     '    if (false) {', ['test_markdown_doc:imagesAreDrawnSmoothly']),
    ('lumerical no zincblende check (F3)', 'multiphysics/fem_lumerical.cpp', '            if (!z) {', '            if (false) {', [L]),
    ('lumerical no wurtzite check (F3)', 'multiphysics/fem_lumerical.cpp', '            if (!w) {', '            if (false) {', [L]),
    ('whole is a cast (F4, build-asan)', 'multiphysics/fem_lumerical_script.cpp',
     '    if (!(x == x) || x <= -2147483648.0) return INT_MIN;\n    if (x >= 2147483647.0) return INT_MAX;\n    return int(x);',
     '    return int(x);', [L]),
    ('reshape sizes unchecked (F4)', 'multiphysics/fem_lumerical_script.cpp',
     'if (!(x >= 0 && x <= 1e7) || x != std::floor(x)) raise', 'if (false) raise', [L]),
    ('superblock unchecked (F6)', 'multiphysics/fem_hdf5.cpp',
     '    if (!fits(at, 12)) return fail(tr("its HDF5 superblock is cut short"));\n', '', [L]),
    ('dataspace and fits as before (F6, build-asan)', [
        ('multiphysics/fem_hdf5.cpp',
         "        if (d != 0 && count > most / d) return std::nullopt;\n        count *= d;\n    }\n    if (size <= 0 || quint64(size) > most || count > most / quint64(size) || count > quint64(available / size) + 1\n        || !fits(data, qint64(count) * size))\n        return std::nullopt;\n    v.raw = a_data.mid(data, qsizetype(count * quint64(size)));",
         "        count *= d;\n    }\n    if (size <= 0 || count > quint64(available / size) + 1 || !fits(data, qint64(count) * size)) return std::nullopt;\n    v.raw = a_data.mid(data, int(count * quint64(size)));"),
        ('multiphysics/fem_hdf5.h', 'return at >= 0 && bytes >= 0 && at <= a_data.size() && bytes <= a_data.size() - at;',
         'return at >= 0 && bytes >= 0 && at + bytes <= a_data.size();')], None, None, [L]),
    ('dataspace product unchecked alone (F6, build-asan)', 'multiphysics/fem_hdf5.cpp',
     '        if (d != 0 && count > most / d) return std::nullopt;\n', '', [L]),
    ('fits adds alone (F6, build-asan)', 'multiphysics/fem_hdf5.h',
     'return at >= 0 && bytes >= 0 && at <= a_data.size() && bytes <= a_data.size() - at;',
     'return at >= 0 && bytes >= 0 && at + bytes <= a_data.size();', [L]),
    ('eigen stop not asked (F5)', 'multiphysics/fem_edge.cpp', '            if (stop && stop()) {\n                out.cancelled = true;',
     '            if (false && stop && stop()) {\n                out.cancelled = true;', [W, R]),
    ('resonator before its factorization (F5)', 'multiphysics/fem_waves.cpp', '    // of 2026-10-16.)\n    if (cancelled()) {',
     '    // of 2026-10-16.)\n    if (false) {', [R]),
    ('resonator after its factorization (F5)', 'multiphysics/fem_waves.cpp',
     '    if (cancelled()) {\n        *error = tr("cancelled");\n        return -1;\n    }\n    // The layers\' own modes',
     '    if (false) {\n        *error = tr("cancelled");\n        return -1;\n    }\n    // The layers\' own modes', [R]),
    ('resonator search not stopped (F5)', 'multiphysics/fem_waves.cpp',
     '{ y = lu.solve(M * x); }, 0, 1e-10, 300, [this] { return cancelled(); });',
     '{ y = lu.solve(M * x); }, 0, 1e-10, 300, [] { return false; });', [R]),
    ('mode analysis after its factorization (F5)', 'multiphysics/fem_waves.cpp',
     '    // each product of the search.)\n    if (cancelled()) {', '    // each product of the search.)\n    if (false) {', [W]),
    ('mode search not stopped (F5)', 'multiphysics/fem_waves.cpp',
     '{ y = lu.solve(B * x); }, 0, 1e-10, 300,\n                                            [this] { return cancelled(); });',
     '{ y = lu.solve(B * x); }, 0, 1e-10, 300,\n                                            [] { return false; });', [W]),
    ('mode search\'s stop ignored (F5)', 'multiphysics/fem_waves.cpp',
     '                                            [this] { return cancelled(); });\n        if (r.cancelled) {',
     '                                            [this] { return cancelled(); });\n        if (false) {', [W]),
]


def run(name, path, old, new, tests):
    # (path, old, new), or a list of them in path.
    edits = path if isinstance(path, list) else [(path, old, new)]
    kept = {}
    for p, o, n in edits:
        src = os.path.join(SRC, p)
        text = kept.get(src) or open(src, encoding='utf-8').read()
        kept.setdefault(src, text)
        now = open(src, encoding='utf-8').read()
        if now.count(o) != 1:
            for f, t in kept.items():
                open(f, 'w', encoding='utf-8').write(t)
            return f'{name}: its text found {now.count(o)} times in {p}'
        open(src, 'w', encoding='utf-8').write(now.replace(o, n))
    try:
        targets = sorted({t.split(':')[0] for t in tests})
        b = subprocess.run(['ninja', '-C', BUILD] + targets, capture_output=True, text=True)
        if b.returncode != 0:
            return f'{name}: the build failed\n{b.stdout[-1500:]}'
        env = dict(os.environ, HOME=H + '/home', QUCS_SETTINGS_DIR=H + '/settings', QUCS_TRASH_DIR=H + '/trash',
                   QUCS_CACHE_DIR=H + '/cache', QT_QPA_PLATFORM='offscreen', ASAN_OPTIONS='detect_leaks=0',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        caught = []
        for t in tests:
            exe, fn = t.split(':')
            p = subprocess.run([os.path.join(BUILD, 'qucs', 'tests', exe), fn], cwd=ROOT, env=env, capture_output=True, text=True,
                               timeout=900)
            fails = [l for l in p.stdout.splitlines() if l.startswith('FAIL!')]
            if p.returncode != 0 or fails:
                caught.append(f'{exe}: ' + (fails[0][:200] if fails else f'exit {p.returncode}'))
        return f'{name}: ' + ('CAUGHT - ' + ' | '.join(caught) if caught else 'NOT CAUGHT')
    finally:
        for f, t in kept.items():
            open(f, 'w', encoding='utf-8').write(t)


for d in ('home', 'settings', 'trash', 'cache'):
    os.makedirs(os.path.join(H, d), exist_ok=True)
for name, path, old, new, tests in BREAKS:
    if len(sys.argv) > 1 and not any(a in name for a in sys.argv[1:]):
        continue
    print(run(name, path, old, new, tests), flush=True)
