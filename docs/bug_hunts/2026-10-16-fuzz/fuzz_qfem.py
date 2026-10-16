# Fuzzes multiphysics models (.qfem) through `qucs-s --mcp-server` (the
# ASan/UBSan build): the shipped examples' JSON damaged - keys taken out,
# values made hostile or of another type, expressions made junk, items
# repeated, the text cut or its bytes flipped - each opened, built, solved
# (its own time limit), described, evaluated and closed. A crash (EOF), a
# call past its time limit (HANG) or a sanitizer report is a finding: the
# mutant and the calls kept in findings/.
#   QUCS=<qucs-s> HUNT_RUNS=<scratch> python3 fuzz_qfem.py SEED COUNT
import json, os, random, re, select, shutil, subprocess, sys, time
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
APP = os.environ['QUCS']   # the app's binary: the ASan/UBSan build's for the reports
EX = os.path.join(ROOT, 'qucs-s-26.1.1', 'examples', 'multiphysics')
H = os.environ['HUNT_RUNS']   # a scratch folder: the servers' workspaces, settings, HOME, cache, trash and the logs
os.makedirs(H, exist_ok=True)
seed, total = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
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
s = Server(); pos = os.path.getsize(errpath); findings = 0; t0 = time.time(); stats = {}
EXPR = ['es.V', 'ht.T', 'semi.n', 'emr.normE', 'mf.Bz', '1/0', 'sqrt(-1)', 'nosuch', 'x*y', 'es.V^1e308', '((', 'T[degC]', 'abs(es.Ex)']
for i in range(total):
    name = rnd.choice(examples)
    text = open(os.path.join(EX, name), encoding='utf-8').read()
    kind = rnd.random()
    if kind < 0.8:
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
    done = []
    bad = None
    for name_, args, limit in calls:
        r, took = s.call(name_, args, limit)
        done.append((name_, round(took, 1)))
        st = stats.setdefault(name_, [0, 0, 0.0]); st[0] += 1; st[2] = max(st[2], took)
        if isinstance(r, dict) and r.get('result', {}).get('isError'): st[1] += 1
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
print(f'[{seed}] {total} mutants, {findings} findings, {time.time() - t0:.0f} s', flush=True)
s.kill()
