# Fuzzes the MCP tools of `qucs-s --mcp-server` (the ASan/UBSan build):
# random calls built from each tool's schema, with names of the parts open,
# ground refs and hostile values. A crash (EOF), a hang (no answer in time)
# or a sanitizer report on stderr is recorded with the calls before it.
#   python3 fuzz_tools.py SEED CALLS
import json, os, random, shutil, subprocess, sys, time, select
H = os.path.dirname(os.path.abspath(__file__))
APP = os.environ.get('QUCS', '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s')
EX = '/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples'
seed, total = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
# Its workspace, a folder of its own per run: --workspace, checked before
# the first call (without it the server worked in ~/QucsWorkspace, and the
# calls' files - projects named "'" or {} - went there). Two folders down in
# runs/: a call given .. or ../.. (open_project, a path) stays in runs/ too.
WS = H + f'/runs/{seed}/w/ws'
SKIP = {'trigger_action', 'clean_scratch', 'set_dialog', 'get_dialog', 'run_script'}
SLOW = {'simulate', 'tune', 'build_verilog_a'}
SEEDS = ['ngspice/RF/Miscellaneous/RCL_resonance.sch', 'ngspice/Analog/Amplifiers/singleOPV.sch' if False else None]
out = open(f'{H}/fuzz-{seed}.log', 'w')
errpath = f'{H}/fuzz-{seed}.stderr'

def pick_examples():
    files = []
    for root, _, fs in os.walk(EX):
        files += [os.path.join(root, f) for f in fs if f.endswith('.sch')]
    files.sort()
    return rnd.sample(files, 3)

class Server:
    def __init__(self):
        self.err = open(errpath, 'a')
        # (A home of its own too: a folder opened as a project keeps its
        # scratch files in the cache, ~/Library/Caches/qucs-s.)
        os.makedirs(WS, exist_ok=True); os.makedirs(H + f'/runs/{seed}/home', exist_ok=True)
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=H + f'/runs/{seed}/settings', HOME=H + f'/runs/{seed}/home', QUCS_NO_SHELL_ENV='1',
                   ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='print_stacktrace=1')
        self.p = subprocess.Popen([APP, '--mcp-server', '--workspace', WS], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err, env=env, bufsize=0)
        self.n = 0; self.buf = b''
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        r = self.rpc('tools/call', {'name': 'get_state', 'arguments': {}})
        try: ws = json.loads(r['result']['content'][0]['text'])['workspace']
        except Exception: ws = repr(r)[:200]
        if os.path.realpath(ws) != os.path.realpath(WS):
            self.kill()
            sys.exit(f'the server works in {ws}, not {WS}: stopped before any call')
    def send(self, o):
        self.p.stdin.write((json.dumps(o) + '\n').encode()); self.p.stdin.flush()
    def line(self, end):
        # (Its own buffer: select() on a buffered pipe waits for lines
        # already read into the buffer - two answers come back together.)
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
    def rpc(self, method, params, timeout=90):
        self.n += 1
        self.send({'jsonrpc': '2.0', 'id': self.n, 'method': method, 'params': params})
        end = time.time() + timeout
        while True:
            line = self.line(end)
            if line in ('HANG', 'EOF'): return line
            try: m = json.loads(line)
            except Exception: continue
            if m.get('id') == self.n: return m
    def kill(self):
        try: self.p.kill()
        except Exception: pass

WEIRD_STR = ['', ' ', 'R1', 'GND', 'GND#0', 'GND#-1', 'GND#99999999999', 'gnd#1', '#', '##', 'R1.0', 'R1.999', 'R1.-1',
             '.', '..', '../..', 'a' * 5000, 'Ωµ', 'NaN', '1e308', '-0', '{}', '[]', '"', "'", '\\', '%1%2', 'ground',
             'selection', 'ngspice/ac.v(out)', 'v(', 'v(out', 'db(v(out))', 'ac.', 'tran.v(', '1kk', '10uu', 'x' * 64,
             'untitled', '*', '<Components>', '\n', '\x00', 'amp.sch', 'none.sch', 'Sub', 'Eqn', 'GND.1', 'GND#2.1', 'ac.v(out)']
WEIRD_INT = [0, 1, -1, 2, 3, 4, 10, 60, 100, -100, 2**31 - 1, -2**31, 2**31, 2**53, -2**53, 10**12, 99999]
WEIRD_NUM = WEIRD_INT + [0.5, -0.5, 1e-300, 1e300, 1e308, -1e308, 3.14159]

class Gen:
    def __init__(self): self.names = ['R1']; self.nets = ['out']
    def string(self, key):
        c = rnd.random()
        if key in ('name', 'component') and c < 0.7: return rnd.choice(self.names)
        if key in ('from', 'to', 'at') and c < 0.6: return rnd.choice(self.names) + '.' + str(rnd.randint(1, 3))
        if key == 'path' and c < 0.8: return rnd.choice(['', 'untitled', 'a.sch', 'nope.sch', '../x.sch', 'sub/b.sch'])
        if key in ('save_as', 'file', 'to', 'as') and c < 0.7: return rnd.choice(['fz_out.sch', 'fz/out.png', 'fz_out.cir', 'fz.dat', 'sub/../fz2.sch'])
        if key == 'variable' and c < 0.7: return rnd.choice(['v(out)', 'ac.v(out)', 'tran.v(out)', 'out', 'nothing', 'i(V1)', 'db(v(out))'])
        return rnd.choice(WEIRD_STR)
    def value(self, key, schema, depth=0):
        t = schema.get('type')
        if 'enum' in schema and rnd.random() < 0.85: return rnd.choice(schema['enum'])
        if isinstance(t, list): t = rnd.choice(t)
        if t is None or rnd.random() < 0.08:
            t = rnd.choice(['string', 'integer', 'number', 'boolean', 'array', 'object', 'null'])
        if t == 'string': return self.string(key)
        if t == 'integer': return rnd.choice(WEIRD_INT) if rnd.random() < 0.5 else rnd.randint(-50, 1000) // 10 * 10
        if t == 'number': return rnd.choice(WEIRD_NUM)
        if t == 'boolean': return rnd.random() < 0.5
        if t == 'null': return None
        if t == 'array':
            if depth > 2: return []
            items = schema.get('items', {})
            n = rnd.choice([0, 1, 2, 2, 3, 4, 50]) if rnd.random() < 0.95 else 2000
            return [self.value(key, items if isinstance(items, dict) else {}, depth + 1) for _ in range(n)]
        if t == 'object':
            if depth > 2: return {}
            props = schema.get('properties')
            if props:
                return {k: self.value(k, v, depth + 1) for k, v in props.items() if rnd.random() < 0.6}
            keys = ['R', 'C', 'L', 'U', 'Temp', 'Symbol', 'variable', 'what', 'x', 'y', 'label', 'k', 'gain', '1', '2', 'in']
            return {rnd.choice(keys): self.value('v', {}, depth + 1) for _ in range(rnd.randint(0, 3))}
        return None
    def args(self, tool):
        props = tool.get('inputSchema', {}).get('properties', {})
        a = {}
        for k, v in props.items():
            if k == 'preview' and rnd.random() < 0.5: continue
            if k == 'path' and rnd.random() < 0.9: continue
            if rnd.random() < 0.4 or k in tool.get('inputSchema', {}).get('required', []):
                a[k] = self.value(k, v)
        if tool['name'] in SLOW:
            a['timeout'] = 10
            if tool['name'] == 'tune': a['max_runs'] = 2
        return a

def setup(s, g):
    files = pick_examples()
    for f in files:
        dst = os.path.join(WS, os.path.basename(f))
        shutil.copy(f, dst)
        s.rpc('tools/call', {'name': 'open_document', 'arguments': {'path': dst}})
    r = s.rpc('tools/call', {'name': 'get_schematic', 'arguments': {}})
    try:
        d = json.loads(r['result']['content'][0]['text'])
        g.names = [c.get('ref') or c.get('name') or c['type'] for c in d['components']][:60] or ['R1']
    except Exception: pass
    return files

def sanitizer_lines(pos):
    with open(errpath, errors='replace') as f:
        f.seek(pos); new = f.read(); end = f.tell()
    hits = [l for l in new.splitlines() if 'runtime error' in l or 'AddressSanitizer' in l or 'ERROR:' in l or 'SUMMARY' in l]
    return hits, end

s = Server(); g = Gen()
tools = [t for t in s.rpc('tools/list', {})['result']['tools'] if t['name'] not in SKIP]
files = setup(s, g)
out.write(json.dumps({'seed': seed, 'examples': files}) + '\n')
recent = []; pos = 0; events = 0; t0 = time.time(); stats = {}; texts = open(f'{H}/fuzz-{seed}.texts', 'w')
for i in range(total):
    tool = rnd.choice(tools)
    a = g.args(tool)
    if tool['name'] in ('new_project', 'open_project') and rnd.random() < 0.7: continue
    recent.append((tool['name'], a)); recent = recent[-8:]
    r = s.rpc('tools/call', {'name': tool['name'], 'arguments': a}, timeout=120 if tool['name'] in SLOW else 90)
    hits, pos = sanitizer_lines(pos)
    if isinstance(r, dict) and 'result' in r:
        err = bool(r['result'].get('isError'))
        st = stats.setdefault(tool['name'], [0, 0]); st[1 if err else 0] += 1
        txt = ' '.join(c.get('text', '') for c in r['result'].get('content', []))
        texts.write(f"{i} {tool['name']} {'ERR' if err else 'ok '} {json.dumps(a)[:300]}\n    -> {txt[:400]}\n")
    bad = None
    if r in ('EOF', 'HANG'): bad = r
    elif hits: bad = 'SANITIZER'
    elif 'error' in r and r['error'].get('code') not in (-32602,): bad = 'RPCERROR ' + json.dumps(r['error'])[:300]
    if bad:
        events += 1
        out.write(json.dumps({'call': i, 'what': bad, 'hits': hits[:12], 'recent': recent}, default=str)[:20000] + '\n'); out.flush()
        print(f'[{seed}] call {i}: {bad} {tool["name"]} {hits[:2]}', flush=True)
        if r in ('EOF', 'HANG'):
            s.kill(); s = Server(); files = setup(s, g); pos = os.path.getsize(errpath)
    elif i % 25 == 0:
        # names of the parts now open, for the next calls
        rr = s.rpc('tools/call', {'name': 'get_schematic', 'arguments': {}})
        try:
            d = json.loads(rr['result']['content'][0]['text'])
            g.names = [c.get('ref') or c.get('name') or c['type'] for c in d['components']][:60] or g.names
        except Exception: pass
print(' '.join(f'{k}:{v[0]}/{v[0]+v[1]}' for k, v in sorted(stats.items())))
print(f'[{seed}] {total} calls, {events} events, {time.time() - t0:.0f} s', flush=True)
s.kill()
