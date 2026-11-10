# Every tool of `qucs-s --mcp-server` called with arguments made from its
# schema - wrong types, missing, huge, hostile, names of what is open - in
# odd orders, with a document of each kind open: two schematics, a data
# display, a 2D and a 3D multiphysics model, an RF 2.5D model, a layout, a
# PDF, a deck, a Markdown file (the hunt of 16 October's fuzz_tools.py, its
# documents of every kind and the tools since). Now and then a document is
# closed or opened again between calls, a slow tool's call left running
# while the next comes (pipelined), or a call's arguments nested deep.
# A crash (EOF), a hang or a sanitizer's report is kept with the calls before.
#   QUCS=<qucs-s> python3 fuzz_tools.py SEEDS OUT SEED SECONDS
import json, os, random, shutil, sys, time
sys.setrecursionlimit(100000)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, sanitizer_lines

APP = os.environ['QUCS']
seeds_dir, out, seed, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rnd = random.Random(seed)
H = os.path.join(out, f'run-{seed}')
shutil.rmtree(H, ignore_errors=True); os.makedirs(H)
errpath = os.path.join(H, 'stderr.txt')
crashes = os.path.join(out, 'crashes'); os.makedirs(crashes, exist_ok=True)
log = open(os.path.join(H, 'calls.txt'), 'w')

# Not called: what reaches the system's trash, the network, a shell, or a
# modal dialog the fuzzer cannot answer (the hunts before skip the same).
SKIP = {'trigger_action', 'clean_scratch', 'set_dialog', 'get_dialog', 'trash_file', 'git_clone', 'git_remote',
        'console', 'new_project', 'open_project'}
SLOW = {'simulate', 'tune', 'build_verilog_a', 'fem_solve', 'fem_build', 'fem_converge', 'fem_optimize', 'rf25_solve',
        'rf25_inductance', 'route_run', 'wait_for', 'fem_evaluate', 'rf25_compare'}
DOCS = ['singleOPV.sch', 'bpf_10Ghz.sch', 'display.dpl', 'coax.qfem', 'tsv.qfem3d', 'model.qrf25',
        'thermo_optic_heater.gds', 'test1.pdf', 'text10.pptx', 'README.md']

WEIRD_STR = ['', ' ', 'R1', 'GND', 'GND#0', 'GND#-1', 'GND#99999999999', '#', 'R1.0', 'R1.-1', '.', '..', '../..',
             'a' * 5000, 'Ωµ', 'NaN', '1e308', '-0', '{}', '[]', '"', "'", '\\', '%1%2', 'v(out', 'db(v(out))', '1kk',
             'untitled', '*', '<Components>', '\n', '\x00', 'none.sch', 'Sub', 'gnd1', 'm1', 'M1', 'M2', 'stack',
             'geometry', 'mesh', 'results', 'r1', 'c1', 'model', 'TOP', '1/0', '31/0', 'all', 'none', '-1', '0', '1e-300',
             '‮', '😀' * 100, 'x' * 70000, '/', '/etc/passwd', '~', '$HOME', '`echo hi`', '${x}', '[1,2', 'true']
WEIRD_INT = [0, 1, -1, 2, 3, 4, 10, 60, 100, -100, 2**31 - 1, -2**31, 2**31, 2**53, -2**53, 10**12, 99999, 2**63, -2**63, 10**30]
WEIRD_NUM = WEIRD_INT + [0.5, -0.5, 1e-300, 1e300, 1e308, -1e308, 3.14159, 5e-324]


class Gen:
    def __init__(self):
        self.names = ['R1']; self.tags = ['geom1']; self.docs = []

    def string(self, key):
        c = rnd.random()
        if key in ('path',) and c < 0.85: return rnd.choice(self.docs + [''] + ['nope.sch', '../x.sch'])
        if key in ('name', 'component', 'ref') and c < 0.6: return rnd.choice(self.names)
        if key in ('tag', 'parent', 'node', 'until', 'of') and c < 0.6: return rnd.choice(self.tags)
        if key in ('from', 'to', 'at') and c < 0.5: return rnd.choice(self.names) + '.' + str(rnd.randint(1, 3))
        if key in ('save_as', 'file', 'as') and c < 0.7:
            return rnd.choice(['fz_out.sch', 'fz/out.png', 'fz_out.cir', 'fz.dat', 'fz.s2p', 'fz.xml', 'fz.proc', 'fz.py',
                               'fz.csv', 'fz.svg', 'fz.pdf', 'fz.qfem', 'fz.gds', 'fz.lsf', 'sub/../fz2.sch'])
        if key == 'variable' and c < 0.7: return rnd.choice(['v(out)', 'ac.v(out)', 'out', 'i(V1)', 'es.C11'])
        return rnd.choice(WEIRD_STR)

    def value(self, key, schema, depth=0):
        t = schema.get('type') if isinstance(schema, dict) else None
        if isinstance(schema, dict) and 'enum' in schema and rnd.random() < 0.85: return rnd.choice(schema['enum'])
        if isinstance(t, list): t = rnd.choice(t)
        if t is None or rnd.random() < 0.08:
            t = rnd.choice(['string', 'integer', 'number', 'boolean', 'array', 'object', 'null'])
        if t == 'string': return self.string(key)
        if t == 'integer': return rnd.choice(WEIRD_INT) if rnd.random() < 0.5 else rnd.randint(-50, 1000) // 10 * 10
        if t == 'number': return rnd.choice(WEIRD_NUM)
        if t == 'boolean': return rnd.random() < 0.5
        if t == 'null': return None
        if t == 'array':
            if depth > 3: return []
            items = schema.get('items', {}) if isinstance(schema, dict) else {}
            n = rnd.choice([0, 1, 2, 2, 3, 4, 50]) if rnd.random() < 0.97 else 3000
            return [self.value(key, items if isinstance(items, dict) else {}, depth + 1) for _ in range(n)]
        if t == 'object':
            if depth > 3: return {}
            props = schema.get('properties') if isinstance(schema, dict) else None
            if props:
                return {k: self.value(k, v, depth + 1) for k, v in props.items() if rnd.random() < 0.6}
            keys = ['R', 'C', 'U', 'Temp', 'x', 'y', 'label', 'size', 'position', 'thickness', 'material', 'expression',
                    'value', 'name', 'from', 'to', 'width', 'height', 'eps_r', 'sigma', '1', 'in']
            return {rnd.choice(keys): self.value('v', {}, depth + 1) for _ in range(rnd.randint(0, 4))}
        return None

    def args(self, tool):
        sch = tool.get('inputSchema', {})
        props = sch.get('properties', {})
        a = {}
        for k, v in props.items():
            if rnd.random() < 0.45 or k in sch.get('required', []):
                a[k] = self.value(k, v)
        if rnd.random() < 0.03: a[rnd.choice(['zzz', 'path ', ''])] = self.value('x', {})
        if rnd.random() < 0.01: a = rnd.choice([[], 'x', 1, None])   # not an object at all
        if isinstance(a, dict):
            a.pop('choose', None) if tool['name'] == 'context_menu' else None
            if tool['name'] in SLOW: a['timeout'] = rnd.choice([1, 5, 10])
            if tool['name'] == 'batch':
                a['calls'] = [c for c in a.get('calls', []) if isinstance(c, dict) and str(c.get('tool', c.get('name'))) not in SKIP] \
                    if isinstance(a.get('calls'), list) else a.get('calls')
        return a


def nested(depth):
    v = 1
    for _ in range(depth): v = [v] if rnd.random() < 0.5 else {'a': v}
    return v


class Run:
    def __init__(self, g):
        self.g = g; self.start()

    def start(self):
        self.s = Server(APP, H, errpath)
        self.pos = os.path.getsize(errpath)
        self.g.docs = []
        for d in DOCS:
            src = os.path.join(seeds_dir, d)
            if not os.path.exists(src): continue
            dst = os.path.join(self.s.ws, d)
            shutil.copy(src, dst)
            if d.endswith('.qrf25'): shutil.copy(os.path.join(seeds_dir, 'thermo_optic_heater.gds'), os.path.join(self.s.ws, 'heater.gds'))
            self.s.call('open_document', {'path': dst}, timeout=180)
            self.g.docs.append(d)
        self.refresh()

    def refresh(self):
        r = self.s.call('get_schematic', {'path': 'singleOPV.sch', 'format': 'json'})
        try:
            d = json.loads(self.s.text(r))
            self.g.names = [c.get('name') or c['type'] for c in d['components']][:60] or ['R1']
        except Exception: pass
        r = self.s.call('fem_describe', {'path': 'coax.qfem'})
        try:
            import re
            self.g.tags = list(dict.fromkeys(re.findall(r'"tag":\s*"([^"]+)"', self.s.text(r))))[:80] or self.g.tags
        except Exception: pass


g = Gen(); run = Run(g)
tools = [t for t in run.s.rpc('tools/list', {})['result']['tools'] if t['name'] not in SKIP]
t0 = time.time(); i = 0; events = 0; stats = {}; recent = []
while time.time() - t0 < seconds:
    i += 1
    c = rnd.random()
    if c < 0.03:
        d = rnd.choice(DOCS)
        name, a = rnd.choice([('close_document', {'path': d, 'unsaved': 'discard'}), ('open_document', {'path': os.path.join(run.s.ws, d)})])
    elif c < 0.04:
        name, a = rnd.choice(tools)['name'], {'path': nested(rnd.choice([50, 2000]))}
    else:
        tool = rnd.choice(tools); name = tool['name']; a = g.args(tool)
    recent.append((name, a)); recent = recent[-10:]
    if c > 0.97 and name in SLOW:
        # pipelined: the next call sent before this one's answer
        run.s.n += 1
        run.s.send({'jsonrpc': '2.0', 'id': run.s.n, 'method': 'tools/call', 'params': {'name': name, 'arguments': a}})
        tool2 = rnd.choice(tools); a2 = g.args(tool2); name = tool2['name']; a = a2
        recent.append((name, a))
    limit = 150 if name in SLOW else 90
    r = run.s.call(name, a, timeout=limit)
    hits, run.pos, _ = sanitizer_lines(errpath, run.pos)
    if isinstance(r, dict) and 'result' in r:
        err = bool(r['result'].get('isError'))
        st = stats.setdefault(name, [0, 0]); st[1 if err else 0] += 1
        log.write(f"{i} {name} {'ERR' if err else 'ok '} {json.dumps(a, default=str)[:300]}\n    -> {run.s.text(r)[:300]}\n")
    bad = None
    if r in ('EOF', 'HANG'): bad = r
    elif hits: bad = 'SANITIZER'
    elif isinstance(r, dict) and 'error' in r and r['error'].get('code') not in (-32602, -32700): bad = 'RPCERROR ' + json.dumps(r['error'])[:300]
    if bad:
        events += 1
        tag = f"{bad.split()[0].lower()}-{seed}-{i}"
        time.sleep(1)
        _, _, tail = sanitizer_lines(errpath, max(0, run.pos - 200000))
        json.dump({'call': i, 'what': bad, 'hits': hits[:20], 'recent': recent}, open(os.path.join(crashes, tag + '.json'), 'w'), indent=1, default=str)
        open(os.path.join(crashes, tag + '.stderr'), 'w').write(tail[-60000:])
        print(f'[{seed}] call {i}: {bad} {name} {hits[:2]}', flush=True)
        if r in ('EOF', 'HANG') or hits:
            run.s.kill(); run.start()
    elif i % 40 == 0:
        run.refresh()
print(' '.join(f'{k}:{v[0]}/{v[0]+v[1]}' for k, v in sorted(stats.items())))
print(f'[{seed}] {i} calls, {events} events, {time.time() - t0:.0f} s', flush=True)
json.dump(stats, open(os.path.join(H, 'stats.json'), 'w'), indent=1)
run.s.kill()
