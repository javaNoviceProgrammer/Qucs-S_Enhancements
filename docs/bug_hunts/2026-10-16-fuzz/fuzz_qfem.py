# Fuzzes multiphysics models (.qfem) through `qucs-s --mcp-server` (the
# ASan/UBSan build): the shipped examples' JSON damaged - keys taken out,
# values made hostile or of another type, expressions made junk, items
# repeated, the text cut or its bytes flipped - each opened, built, solved
# (its own time limit), described, evaluated and closed. A crash (EOF), a
# call past its time limit (HANG) or a sanitizer report is a finding: the
# mutant and the calls kept in findings/.
# Of its mutants one in three (QFEM_OPT_SHARE) is an optimization's: the
# "optimizations" section (docs/feature_gaps/2026-11-07-multiphysics-optimization.md)
# added to a shipped example or to a small capacitor that solves in
# milliseconds, the section alone damaged - its settings, its rows, a kept
# result's rows - and run by fem_optimize, read, written in and stopped too.
# Of the rest, one in five (QFEM_CONV_SHARE) is a mesh convergence check's: the
# "convergences" section (docs/feature_gaps/2026-11-07-multiphysics-mesh-convergence.md)
# added to a small model or a shipped one, damaged alone (its settings, its
# rows, a kept result's rows and sizes), a damaged Adapted Sizes feature in the
# mesh node sometimes, then run by fem_converge's adaptive method, stopped,
# read, its refined mesh kept and built, and saved.
#   QUCS=<qucs-s> HUNT_RUNS=<scratch> python3 fuzz_qfem.py SEED COUNT
import json, os, random, re, select, shutil, subprocess, sys, time
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
APP = os.environ['QUCS']   # the app's binary: the ASan/UBSan build's for the reports
EX = os.path.join(ROOT, 'qucs-s-26.1.1', 'examples', 'multiphysics')
H = os.environ['HUNT_RUNS']   # a scratch folder: the servers' workspaces, settings, HOME, cache, trash and the logs
os.makedirs(H, exist_ok=True)
seed, total = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
OPT_SHARE = float(os.environ.get('QFEM_OPT_SHARE', '0.33'))
CONV_SHARE = float(os.environ.get('QFEM_CONV_SHARE', '0.2'))
RUN = os.path.join(H, f'runs/{seed}')
WS = os.path.join(RUN, 'ws')
FIND = os.path.join(H, 'findings')
os.makedirs(WS, exist_ok=True); os.makedirs(FIND, exist_ok=True)
errpath = os.path.join(RUN, 'stderr.txt')
log = open(os.path.join(H, f'qfem-{seed}.log'), 'w')

HOSTILE = [0, -1, 1, 2**31 - 1, -2**31, 2**53, 1e308, -1e308, 1e-320, 0.5, -0.5, 3.0e9, None, True, False, '', ' ', 'nan', 'inf',
           '-inf', '1/0', '0/0', 'sqrt(-1)', '1e400[m]', '((((', 'x^', '1[nosuchunit]', 'pi*e', 'w/0', '1e-9[m]', '1e-12[m]',
           '1e9[m]', '-1[mm]', 'a' * 3000, 'Ωµ', '{}', '[]', 'T', 'x', 'y', 'r', 'z', 'nosuch', [], {}, [0], [1e308, -1e308],
           ['a', 'b'], {'of': ['nosuch']}, {'numbers': [99999, -1, 0]}, {'numbers': []}]

# An optimization's section: every setting and table, what a row may hold.
OPT_METHODS = ['auto', 'de', 'pso', 'sa', 'cmaes', 'bayes', 'nm', 'tr', 'lm', 'nsga2', 'tune']
OPT_TYPES = ['LIN_DOUBLE', 'LOG_DOUBLE', 'LIN_INT', 'LOG_INT', 'E3', 'E6', 'E12', 'E24', 'E48', 'E96', 'E192']
OPT_KINDS = ['MIN', 'MAX', 'TARGET', 'LE', 'GE', 'EQ', 'MON']
OPT_EXPR = ['es.C11', 'es.W', 'ht.Tmax', 'ec.R', 'd', 'w*d', 'nosuch', '1/0', 'es.normE', 'x', 'es.C11/1[pF]', '((', '0/0', '1e308*1e308']
CAPACITOR = {'format': 'qucs-s multiphysics 1',
             'parameters': [{'name': 'w', 'expression': '4[mm]'}, {'name': 'd', 'expression': '1[mm]'}, {'name': 'er', 'expression': '4'}],
             'components': [{'unit': 'mm', 'geometry': [{'type': 'rectangle', 'name': 'gap', 'size': ['w', 'd']}],
                             'materials': [{'label': 'dielectric', 'epsilonr': 'er', 'selection': {'all': True}}],
                             'physics': [{'type': 'electrostatics', 'tag': 'es', 'order': '1', 'features': [
                                 {'type': 'terminal', 'name': '1', 'V0': '5[V]', 'selection': {'of': ['gap'], 'side': 'top'}},
                                 {'type': 'ground', 'selection': {'of': ['gap'], 'side': 'bottom'}}]}],
                             'mesh': {'size': 'normal', 'custom': True, 'hmax': 'max(d, w)/4'}}],
             'studies': [{'type': 'study', 'tag': 'std1', 'steps': [{'type': 'stationary'}]}]}

def opt_section(d):
    """An optimization of model \a d's own parameters, within a small budget."""
    names = [p.get('name') for p in d.get('parameters', []) if isinstance(p, dict) and p.get('name')] or ['d']
    method = rnd.choice(OPT_METHODS)
    good = {'w': ['1[mm]', '9[mm]'], 'd': ['0.2[mm]', '3[mm]'], 'er': ['1', '9']}
    rows = []
    for name in rnd.sample(names, min(len(names), rnd.randint(1, 3))):
        # (Most of them a range that holds, so that it runs and what follows it is reached.)
        lo, hi = good.get(name, ['0.5', '10']) if rnd.random() < 0.95 else [rnd.choice(['0.5', '1', '0.2[mm]', '1[V]']), rnd.choice(['10', '5', '3[mm]', '9[V]'])]
        rows.append([name, rnd.choice(['', '', '', lo]), lo, hi, rnd.choice(['LIN_DOUBLE', 'LOG_DOUBLE', ''] if rnd.random() < 0.85 else OPT_TYPES), rnd.choice(['yes', 'yes', 'yes', 'no', ''])])
    measured = [['es.C11', 'TARGET', '200[pF]'], ['es.C11', 'MIN'], ['es.W', 'MAX'], ['w*d', 'MIN'], ['es.W', 'LE', '1[uJ]'], ['es.C11', 'GE', '50[pF]'], ['es.W', 'MON'], ['es.C11', 'EQ', '100[pF]']]
    objectives = [rnd.choice(measured) if rnd.random() < 0.95 else [rnd.choice(OPT_EXPR), rnd.choice(OPT_KINDS), rnd.choice(['', '1', '200[pF]', '350[K]', '1[ohm]']), rnd.choice(['', '1', '2']), rnd.choice(['', 'worst', 'last'])]
                  for _ in range(rnd.randint(1, 3))]
    if method == 'nsga2': objectives += [['es.C11', 'MAX'], ['w*d', 'MIN']]
    if method == 'tune': objectives = [['es.C11', 'TARGET', '200[pF]']] + objectives[1:]
    section = {'type': 'optimization', 'tag': 'opt1', 'method': method, 'maxiter': rnd.choice([1, 2, 3, 5, 8]), 'population': rnd.choice([0, 4, 6, 8]), 'seed': rnd.randint(0, 99),
               'variables': rows, 'objectives': objectives}
    extra = {'polish': [True, False], 'starts': [0, 1], 'tol': ['1e-3', '1e-6'], 'ctol': ['1e-2', '1e-4'], 'constraints': ['lagrangian', 'penalty'],
             'parallel': [1, 2, 3], 'refresh': [0, 1], 'strategy': ['DE/best/1/exp', 'DE/rand/2/bin'], 'F': ['0.5', '0.9'], 'CR': ['0', '0.5', '1'],
             'mincostvariance': ['0', '1e-3'], 'fdstep': ['1e-3', '1e-1'], 'objectiveweight': ['1', '10'], 'constraintweight': ['1', '100']}
    for key, values in extra.items():
        if rnd.random() < 0.3: section[key] = rnd.choice(values)
    if method in ('auto', 'nm', 'tr', 'de', 'cmaes') and rnd.random() < 0.15:
        section['objectives'] = [['es.C11', 'LE', '300[pF]'], ['es.C11', 'GE', '50[pF]']]
        section.update({'mode': 'center', 'samples': rnd.choice([2, 3, 4]), 'lhs': rnd.choice([True, False]), 'variation': [[rnd.choice(names), rnd.choice(['gauss', 'uniform']), rnd.choice(['5 %', '0.1[mm]', '0.1'])]]})
    return section

def opt_result():
    """A kept result, as a run writes one, for the section to carry into its damage."""
    return {'method': 'de', 'status': rnd.choice(['converged', 'nosolve', 'infeasible', 'maxiter', 'interrupted']), 'summary': 'x', 'setup': 'x', 'settled': True, 'feasible': True,
            'evaluations': 3, 'solves': 3, 'failed': 0, 'seconds': 0.1, 'parameters': [{'name': 'd', 'text': '1 mm', 'value': 1e-3}], 'measured': [{'name': 'es.C11', 'text': '1 pF', 'value': 1e-12}],
            'kinds': ['MIN'], 'units': ['m', 'F'], 'columns': ['d', 'es.C11'], 'history': ['1 0.001 1e-12 1 1 1', '2 0.002 5e-13 0.5 1 1', 'x y z', '3 nan nan nan 0 0'],
            'front': [{'values': [1e-3], 'measured': [1e-12]}] if rnd.random() < 0.5 else []}

# A mesh convergence check's section: every setting, what a row may hold.
CONV_EXPR = ['es.C11', 'es.W', 'ht.Tmax', 'ec.R', 'nosuch', '1/0', 'es.normE', 'x', 'es.C11/1[pF]', '((', '0/0', '1e308*1e308', 'w*d', 'sensor', 'emw.neff']
CORNER = {'format': 'qucs-s multiphysics 1',
          'components': [{'unit': 'mm', 'geometry': [{'type': 'rectangle', 'name': 'square', 'size': ['2', '2']},
                                                     {'type': 'rectangle', 'name': 'cut', 'position': ['1', '1'], 'size': ['1', '1']},
                                                     {'type': 'difference', 'name': 'plate', 'input': ['square'], 'tools': ['cut']}],
                          'materials': [{'label': 'm', 'epsilonr': '1', 'k': '1[W/(m*K)]', 'selection': {'all': True}}],
                          'physics': [{'type': 'electrostatics', 'tag': 'es', 'order': '1', 'features': [
                              {'type': 'terminal', 'name': '1', 'V0': '1[V]', 'selection': {'box': [0.99, 0.99, 2.01, 2.01]}},
                              {'type': 'ground', 'selection': {'box': [-0.01, -0.01, 0.01, 2.01]}}]}],
                          'mesh': {'size': 'normal', 'custom': True, 'hmax': '0.25'}}],
          'studies': [{'type': 'study', 'tag': 'std1', 'steps': [{'type': 'stationary'}]}]}

def conv_section():
    """A check of a few values, within a small budget."""
    rows = []
    for _ in range(rnd.randint(1, 3)):
        e = rnd.choice(['es.W', 'es.C11']) if rnd.random() < 0.8 else rnd.choice(CONV_EXPR)
        rows.append([e, rnd.choice(['', '', '', '1e-2', '0', '2', 'x']), rnd.choice(['', '', '', '1e-12[J]', '-1', 'nosuch', '1[K]'])])
    section = {'type': 'convergence', 'tag': 'conv1', 'tolerance': rnd.choice(['1e-2', '1e-3', '1e-4', '0', '1', 'x', '1e-12']),
               'maxmeshes': rnd.choice([2, 3, 4, 6, 1, 41]), 'maxunknowns': rnd.choice(['2e4', '5e4', '1e5', '50', '1e10']), 'values': rows}
    extra = {'refine': ['auto', 'goal', 'uniform', 'bogus'], 'check': [True, False, 'x'], 'sweep': ['ends', 'own', 'all', 'middle'],
             'physics': [['es'], [], ['nosuch'], 'es'], 'study': ['std1', '', 'std9']}
    for key, values in extra.items():
        if rnd.random() < 0.3: section[key] = rnd.choice(values)
    return section

def conv_result():
    """A kept result, as a run writes one, for the section to carry into its damage."""
    return {'status': rnd.choice(['converged', 'meshes', 'unknowns', 'cancelled', 'failed', 'x']), 'converged': rnd.choice([True, False]), 'summary': 'x', 'setup': 'x',
            'method': 'x', 'evidence': ['a', 'b'], 'columns': ['es.W'], 'units': ['J'], 'rows': ['1 0 start 174 106 0.1 0.3 0.2 1.1e-11', '2 0 adaptive 245 145 0.09 0 nan 1.13e-11', 'x y', '3 0 check 1 2 3 4 5 inf'],
            'values': [{'column': 0, 'value': 1.13e-11, 'limit': 1.13e-11, 'error': 'inf', 'relative': 'nan', 'rate': 1, 'tolerance': 0.01, 'trust': 'bounded', 'why': 'x', 'settled': True},
                       {'column': 5}], 'solves': 3, 'sizes': ['0.5 0.5 0.01', '1 1 0.001', 'x y z', '1 2', '1e308 1e308 1e-308', '0 0 -1'], 'smallestsize': rnd.choice([0.001, 0, -1, 1e-300])}

def mutate_json(d, n):
    paths = []
    def walk(o, p):
        paths.append(p)
        if isinstance(o, dict):
            for k in list(o)[:200]: walk(o[k], p + [k])
        elif isinstance(o, list):
            for i in range(min(len(o), 200)): walk(o[i], p + [i])
    walk(d, [])
    ops = []
    for _ in range(n):
        p = rnd.choice(paths[1:] or [[]])
        parent = d
        try:
            for k in p[:-1]: parent = parent[k]
        except Exception:
            continue
        key = p[-1] if p else None
        op = rnd.choice(['del', 'set', 'set', 'set', 'dup', 'type', 'expr'])
        try:
            if op == 'del':
                if isinstance(parent, dict): del parent[key]
                elif isinstance(parent, list): parent.pop(key)
            elif op == 'set':
                parent[key] = rnd.choice(HOSTILE)
            elif op == 'dup' and isinstance(parent, list):
                parent.insert(key, json.loads(json.dumps(parent[key])))
            elif op == 'type':
                v = parent[key]
                parent[key] = [v] if not isinstance(v, list) else (v[0] if v else {})
            elif op == 'expr' and isinstance(parent[key], str):
                s = parent[key]
                cut = rnd.randint(0, len(s))
                parent[key] = s[:cut] + rnd.choice(['*', '/', '^', '(', ')', '[', ']', '0', '1e300', ' nosuch ', ',']) + s[cut:]
            ops.append(f'{op} {"/".join(map(str, p))[:120]}')
        except Exception:
            pass
    return ops

class Server:
    def __init__(self):
        self.err = open(errpath, 'a')
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=os.path.join(RUN, 'settings'), HOME=os.path.join(RUN, 'home'),
                   QUCS_CACHE_DIR=os.path.join(RUN, 'cache'), QUCS_TRASH_DIR=os.path.join(RUN, 'trash'), QUCS_NO_SHELL_ENV='1',
                   ASAN_OPTIONS='detect_leaks=0:allocator_may_return_null=1', UBSAN_OPTIONS='print_stacktrace=1')
        for d in ('settings', 'home', 'cache', 'trash'): os.makedirs(os.path.join(RUN, d), exist_ok=True)
        self.p = subprocess.Popen([APP, '--mcp-server', '--workspace', WS], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err, env=env, bufsize=0)
        self.n = 0; self.buf = b''
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
    def send(self, o):
        self.p.stdin.write((json.dumps(o) + '\n').encode()); self.p.stdin.flush()
    def line(self, end):
        while b'\n' not in self.buf:
            left = end - time.time()
            if left <= 0: return 'HANG'
            r, _, _ = select.select([self.p.stdout], [], [], left)
            if not r: return 'HANG'
            chunk = os.read(self.p.stdout.fileno(), 1 << 20)
            if not chunk: return 'EOF'
            self.buf += chunk
        l, _, self.buf = self.buf.partition(b'\n')
        return l.decode('utf-8', 'replace')
    def rpc(self, method, params, timeout=120):
        self.n += 1
        self.send({'jsonrpc': '2.0', 'id': self.n, 'method': method, 'params': params})
        end = time.time() + timeout
        while True:
            line = self.line(end)
            if line in ('HANG', 'EOF'): return line
            try: m = json.loads(line)
            except Exception: continue
            if m.get('id') == self.n: return m
    def call(self, name, args, timeout):
        t0 = time.time()
        r = self.rpc('tools/call', {'name': name, 'arguments': args}, timeout)
        return r, time.time() - t0
    def kill(self):
        try: self.p.kill(); self.p.wait(10)
        except Exception: pass

def sanitizer(pos):
    with open(errpath, errors='replace') as f:
        f.seek(pos); new = f.read(); end = f.tell()
    hits = [l for l in new.splitlines() if 'runtime error' in l or 'AddressSanitizer' in l or 'SUMMARY' in l or 'ASSERT' in l]
    return hits, end

examples = sorted(f for f in os.listdir(EX) if f.endswith('.qfem'))
s = Server(); pos = os.path.getsize(errpath); findings = 0; t0 = time.time(); stats = {}; refused = {}
EXPR = ['es.V', 'ht.T', 'semi.n', 'emr.normE', 'mf.Bz', '1/0', 'sqrt(-1)', 'nosuch', 'x*y', 'es.V^1e308', '((', 'T[degC]', 'abs(es.Ex)']
for i in range(total):
    name = rnd.choice(examples)
    text = open(os.path.join(EX, name), encoding='utf-8').read()
    kind = rnd.random()
    optimized = rnd.random() < OPT_SHARE
    checked = not optimized and rnd.random() < CONV_SHARE
    if checked:
        # The check alone damaged, on a model that solves in milliseconds (or a
        # shipped one), a damaged Adapted Sizes feature in the mesh now and then.
        d = json.loads(json.dumps(rnd.choice([CORNER, CAPACITOR]))) if rnd.random() < 0.85 else json.loads(text)
        section = conv_section()
        if rnd.random() < 0.3: section['result'] = conv_result()
        ops = mutate_json(section, rnd.choice([0, 0, 0, 1, 1, 2, 3]))
        d.setdefault('convergences', []).append(section)
        if rnd.random() < 0.1: d['convergences'].append(rnd.choice([5, 'x', {}, {'type': 'nosuch'}, {'type': 'convergence'}, section]))
        if rnd.random() < 0.2:
            mesh = d['components'][0].setdefault('mesh', {})
            feature = {'type': 'adapted', 'hmin': rnd.choice(['0.001', '0', '-1', 'x', '']), 'points': rnd.choice([['0.5 0.5 0.01', '1 1 1e-9', 'x', '1 2 3 4'], 'x', [5], [], ['1e308 1 1']])}
            features = mesh.setdefault('features', [])
            if isinstance(features, list): features.append(feature)
        name = 'small' if d.get('studies') and d['components'][0].get('mesh', {}).get('hmax') in ('0.25', 'max(d, w)/4') else name
        text = json.dumps(d, indent=1)
    elif optimized:
        # The section alone damaged, on a model that solves in milliseconds (or
        # a shipped one, its solves perhaps past the call's time limit).
        d = json.loads(json.dumps(CAPACITOR)) if rnd.random() < 0.9 else json.loads(text)
        section = opt_section(d)
        if rnd.random() < 0.3: section['result'] = opt_result()
        ops = mutate_json(section, rnd.choice([0, 0, 0, 0, 1, 1, 2, 3]))
        d.setdefault('optimizations', []).append(section)
        if rnd.random() < 0.1: d['optimizations'].append(rnd.choice([5, 'x', {}, {'type': 'nosuch'}, {'type': 'optimization'}, section]))
        name = 'capacitor' if d.get('parameters') == CAPACITOR['parameters'] else name
        text = json.dumps(d, indent=1)
    elif kind < 0.8:
        d = json.loads(text)
        ops = mutate_json(d, rnd.randint(1, 4))
        text = json.dumps(d, indent=1)
    else:
        b = bytearray(text.encode())
        ops = []
        for _ in range(rnd.randint(1, 3)):
            at = rnd.randrange(len(b))
            c = rnd.random()
            if c < 0.4: b[at] = rnd.randrange(256); ops.append(f'flip {at}')
            elif c < 0.7: del b[at:]; ops.append(f'cut {at}'); break
            else: b[at:at] = b[at:at + rnd.randint(1, 200)]; ops.append(f'repeat {at}')
        text = bytes(b).decode('utf-8', 'replace')
    path = os.path.join(WS, f'm{i % 20}.qfem')
    open(path, 'w', encoding='utf-8').write(text)
    calls = [('open_document', {'path': path}, 120),
             ('fem_build', {'path': path, 'timeout': 20}, 120),
             ('fem_solve', {'path': path, 'timeout': 20}, 150),
             ('fem_describe', {'path': path, 'what': rnd.choice(['model', 'geometry', 'variables'])}, 120),
             ('fem_evaluate', {'path': path, 'expression': rnd.choice(EXPR)}, 120),
             ('close_document', {'path': path, 'unsaved': 'discard'}, 120)]
    if optimized:
        # (Instead of the plain solve: a run, its stop, what it kept, written in.)
        calls[2:3] = [('fem_optimize', {'path': path, 'timeout': 10, 'history': rnd.choice([0, 5, -1])}, 120),
                      ('fem_stop', {'path': path, 'timeout': 60}, 120),
                      ('fem_optimize', {'path': path, 'action': 'result', 'history': rnd.choice([0, -1])}, 120),
                      ('fem_optimize', {'path': path, 'action': 'apply', 'point': rnd.choice([1, 2, 1000])}, 120),
                      ('save_document', {'path': path}, 120)]
    if checked:
        # (Instead of the plain solve: a check, its stop, what it kept, its mesh
        # kept and built.)
        calls[2:3] = [('fem_converge', {'path': path, 'method': 'adaptive', 'timeout': 10}, 120),
                      ('fem_stop', {'path': path, 'timeout': 60}, 120),
                      ('fem_converge', {'path': path, 'method': 'adaptive', 'action': 'result'}, 120),
                      ('fem_converge', {'path': path, 'method': 'adaptive', 'action': 'keep'}, 120),
                      ('fem_build', {'path': path, 'timeout': 20}, 120),
                      ('fem_converge', {'path': path, 'method': 'adaptive', 'values': [rnd.choice(CONV_EXPR)], 'max_meshes': 3, 'by': rnd.choice(['auto', 'goal', 'uniform']),
                                        'max_unknowns': rnd.choice([200, 2e4]), 'timeout': 10}, 120),
                      ('fem_stop', {'path': path, 'timeout': 60}, 120),
                      ('save_document', {'path': path}, 120)]
    done = []
    bad = None
    for name_, args, limit in calls:
        r, took = s.call(name_, args, limit)
        done.append((name_, round(took, 1)))
        st = stats.setdefault(name_, [0, 0, 0.0]); st[0] += 1; st[2] = max(st[2], took)
        if isinstance(r, dict) and r.get('result', {}).get('isError'):
            st[1] += 1
            # (What fem_optimize refused and why, in the summary: that the runs do run.)
            if name_ in ('fem_optimize', 'fem_converge'):
                said = re.sub(r'[0-9.e+-]+', '#', str(r['result'].get('content', [{}])[0].get('text', '')))[:70]
                refused[said] = refused.get(said, 0) + 1
        elif name_ in ('fem_optimize', 'fem_converge') and args.get('action') is None and isinstance(r, dict):
            refused['(ran)'] = refused.get('(ran)', 0) + 1
        # (A call past 10 s kept beside the findings, not one: a close that
        # waited 45 s for a mesh's smoothing was seen only as the longest -
        # the hunt of 19 October, B2 and T2.)
        if took > 10:
            keep = os.path.join(FIND, f'slow-{seed}-{i}-{name_}')
            os.makedirs(keep, exist_ok=True)
            open(os.path.join(keep, 'mutant.qfem'), 'w', encoding='utf-8').write(text)
            json.dump({'from': name, 'ops': ops, 'slow': name_, 'took': took, 'calls': done}, open(os.path.join(keep, 'slow.json'), 'w'), indent=1, default=str)
        hits, pos = sanitizer(pos)
        if r in ('EOF', 'HANG'): bad = f'{r} in {name_}'
        elif hits: bad = 'SANITIZER ' + hits[0][:200]
        if bad: break
    if bad:
        findings += 1
        keep = os.path.join(FIND, f'{seed}-{i}')
        os.makedirs(keep, exist_ok=True)
        open(os.path.join(keep, 'mutant.qfem'), 'w', encoding='utf-8').write(text)
        json.dump({'from': name, 'ops': ops, 'what': bad, 'calls': done, 'hits': hits[:20]}, open(os.path.join(keep, 'finding.json'), 'w'), indent=1)
        print(f'[{seed}] mutant {i} ({name}, {ops}): {bad}', flush=True)
        log.write(json.dumps({'mutant': i, 'from': name, 'ops': ops, 'what': bad, 'calls': done}) + '\n'); log.flush()
        s.kill(); s = Server(); pos = os.path.getsize(errpath)
    if i % 20 == 0: print(f'[{seed}] {i} mutants, {findings} findings, {time.time() - t0:.0f} s', flush=True)
print(' '.join(f'{k}:{v[0]} calls, {v[1]} refused, longest {v[2]:.1f} s' for k, v in sorted(stats.items())))
for why, count in sorted(refused.items(), key=lambda kv: -kv[1])[:25]: print(f'  fem_optimize, fem_converge {count}: {why}')
print(f'[{seed}] {total} mutants, {findings} findings, {time.time() - t0:.0f} s', flush=True)
s.kill()
