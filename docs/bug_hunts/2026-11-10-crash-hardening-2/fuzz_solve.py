# Hostile values, not damaged bytes: a model's or schematic's numbers made
# 0, -1, 1e308, NaN, inf, tiny, empty or an expression of them, the file left
# whole so it is read - then it is solved, which the hunt of 9 November never
# reached (its files mostly stopped at the reader). In the real program
# (`qucs-s --mcp-server`, offscreen):
#   .qfem/.qfem3d  fem_build geometry and mesh, fem_solve, fem_evaluate, fem_plot
#   .qrf25         rf25_solve (quasi-static or full wave)
#   .sch           simulate (qucsator or ngspice), get_dataset
# then closed. A crash (EOF), a hang past the tool's own timeout, a sanitizer's
# report or a server past MEM_GB is kept with the file and the calls.
#   QUCS=<qucs-s> [ONLY=.qrf25,...] python3 fuzz_solve.py SEEDS OUT SEED SECONDS
# SEEDS holds the seeds, each in a folder with what it needs beside it.
import json, os, random, re, shutil, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '2026-11-09-crash-hardening'))
from mcp import Server, sanitizer_lines

APP = os.environ['QUCS']
MEM_GB = float(os.environ.get('MEM_GB', '8'))
seeds_dir, out, seed, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rnd = random.Random(seed)
H = os.path.join(out, f'run-{seed}')
shutil.rmtree(H, ignore_errors=True); os.makedirs(H)
errpath = os.path.join(H, 'stderr.txt')
crashes = os.path.join(out, 'crashes'); os.makedirs(crashes, exist_ok=True)
log = open(os.path.join(H, 'log.txt'), 'w')

seeds = sorted(os.path.join(r, f) for r, _, fs in os.walk(seeds_dir) for f in fs
               if f.endswith(('.qfem', '.qfem3d', '.qrf25', '.sch')))
if os.environ.get('ONLY'):   # ONLY=.qrf25,.qfem3d: those kinds alone
    seeds = [s for s in seeds if s.endswith(tuple(os.environ['ONLY'].split(',')))]

HOSTILE = ['0', '-1', '-0', '1e308', '-1e308', '1e-308', 'nan', 'NaN', 'inf', '-inf', '1e30', '1e-30',
           '2147483648', '-2147483649', '1/0', '0/0', '', ' ', 'x', '1e400', '-1e-12', 'sqrt(-1)', '1e15',
           '0.0000001', '9223372036854775808', '1e-320', '((((((((((1))))))))))', '1[', '[V]', '1e308[um]']
WORDS = {'type', 'label', 'tag', 'material', 'format', 'title', 'description', 'name', 'dopant', 'unit',
         'kind', 'solver', 'from', 'to', 'layer', 'file', 'study', 'physics', 'shape', 'operation'}


def hostile(v):
    r = rnd.random()
    if r < 0.6 or not v: return rnd.choice(HOSTILE)
    if r < 0.8: return f'({v})*{rnd.choice(["1e300", "0", "-1", "1e-300", "nan"])}'
    return f'{v}{rnd.choice(["/0", "+1e308", "^1000", "*0", "e999"])}'


def numeric_slots(o, path=()):
    """every place in the JSON whose value is a number, or a string with a digit"""
    if isinstance(o, dict):
        for k, v in o.items():
            if k in WORDS: continue
            if isinstance(v, (dict, list)): yield from numeric_slots(v, path + (k,))
            elif isinstance(v, bool): continue
            elif isinstance(v, (int, float)) or (isinstance(v, str) and re.search(r'\d', v) and len(v) < 80):
                yield (o, k)
    elif isinstance(o, list):
        for i, v in enumerate(o):
            if isinstance(v, (dict, list)): yield from numeric_slots(v, path + (i,))
            elif isinstance(v, (int, float)) and not isinstance(v, bool) or (isinstance(v, str) and re.search(r'\d', v) and len(v) < 80):
                yield (o, i)


def mutate_json(text):
    o = json.loads(text)
    slots = list(numeric_slots(o))
    if not slots: return text, []
    did = []
    for c, k in rnd.sample(slots, min(len(slots), rnd.choice([1, 1, 1, 2, 3, 5]))):
        old = c[k]
        new = hostile(str(old))
        # a JSON number stays one now and then, the reader's own path
        if isinstance(old, (int, float)) and rnd.random() < 0.5:
            new = rnd.choice([0, -1, 1e308, -1e308, 1e-308, 2**31, 2**63, -2**31, 10**30])
        c[k] = new
        did.append(f'{k}: {old!r} -> {new!r}')
    return json.dumps(o, indent=1), did


QUOTED = re.compile(r'"([^"\n]*\d[^"\n]*)"')


def mutate_sch(text):
    lines = text.split('\n')
    # the parts' and the simulations' lines
    idx = [i for i, l in enumerate(lines) if l.startswith('  <') and not l.startswith('  </') and QUOTED.search(l)
           and not l.startswith(('  <Text', '  <Rectangle', '  <Ellipse', '  <Line', '  <Arrow', '  <Arc'))]
    did = []
    if not idx: return text, did
    for i in rnd.sample(idx, min(len(idx), rnd.choice([1, 1, 2, 3]))):
        ms = list(QUOTED.finditer(lines[i]))
        m = rnd.choice(ms)
        new = hostile(m.group(1)).replace('"', '')
        did.append(f'{lines[i][:40]}...: {m.group(1)!r} -> {new!r}')
        lines[i] = lines[i][:m.start(1)] + new + lines[i][m.end(1):]
    return '\n'.join(lines), did


def plan(ext, name):
    if ext in ('.qfem', '.qfem3d'):
        t = [('fem_build', {'path': name, 'what': 'geometry', 'timeout': 30}, 90),
             ('fem_build', {'path': name, 'what': 'mesh', 'timeout': 60}, 150)]
        if rnd.random() < 0.85:
            t.append(('fem_solve', {'path': name, 'timeout': 40,
                                    **({'solver': rnd.choice(['auto', 'direct', 'iterative'])} if ext == '.qfem3d' else {})}, 160))
            t += rnd.sample([('fem_evaluate', {'path': name, 'expression': rnd.choice(['V', 'T', 'es.normE', 'n', 'p', 'neff', 'abs(E)', '1/0'])}, 90),
                             ('fem_evaluate', {'path': name, 'expression': 'V', 'over': 'domain', 'kind': rnd.choice(['integral', 'maximum'])}, 90),
                             ('fem_plot', {'path': name}, 90),
                             ('fem_describe', {'path': name, 'what': 'model'}, 60)], rnd.randint(0, 3))
        return t
    if ext == '.qrf25':
        return [('rf25_solve', {'path': name, 'timeout': 40, 'solver': rnd.choice(['quasi_static', 'full_wave']),
                                **({'frequency': rnd.choice(HOSTILE)} if rnd.random() < 0.2 else {})}, 160),
                ('fem_describe', {'path': name, 'what': 'model'}, 60)]
    sim = rnd.choice(['qucsator', 'ngspice']) if '/ngspice-' not in name else 'ngspice'
    return [('simulate', {'path': name, 'simulator': sim, 'timeout': 40, 'brief': True}, 160),
            ('get_dataset', {'path': name, 'simulator': sim}, 90)] + \
           ([('show_data_display', {'path': name}, 60)] if rnd.random() < 0.3 else [])


class Watch(threading.Thread):
    """kills a server past MEM_GB, and says so"""
    def __init__(self):
        super().__init__(daemon=True); self.pid = None; self.over = 0
    def run(self):
        while True:
            time.sleep(2)
            pid = self.pid
            if not pid: continue
            try: rss = int(subprocess.run(['ps', '-o', 'rss=', '-p', str(pid)], capture_output=True, text=True).stdout.strip() or 0)
            except Exception: continue
            if rss > MEM_GB * 1024 * 1024:
                self.over = rss
                try: os.killpg(pid, 9)
                except Exception: pass


watch = Watch(); watch.start()


class Run:
    def __init__(self): self.start()
    def start(self):
        self.s = Server(APP, H, errpath)
        watch.pid = self.s.p.pid; watch.over = 0
        self.pos = os.path.getsize(errpath)


run = Run()
t0 = time.time(); k = 0; events = 0; stats = {}
while time.time() - t0 < seconds:
    k += 1
    src = rnd.choice(seeds)
    ext = os.path.splitext(src)[1]
    text = open(src, encoding='utf-8', errors='replace').read()
    try:
        data, did = mutate_json(text) if ext != '.sch' else mutate_sch(text)
    except Exception as e:
        continue
    d = os.path.join(run.s.ws, f'c{k % 5}', os.path.basename(os.path.dirname(src)))
    shutil.rmtree(os.path.dirname(d), ignore_errors=True)
    shutil.copytree(os.path.dirname(src), d, ignore=shutil.ignore_patterns('*.dat*', '*.dpl'))
    name = os.path.join(d, 'fz_' + os.path.basename(src))
    open(name, 'w').write(data)
    calls = [('open_document', {'path': name}, 120)] + plan(ext, name) + [('close_document', {'path': name, 'unsaved': 'discard'}, 120)]
    what = None
    done = []
    for tool, args, timeout in calls:
        tc = time.time()
        r = run.s.call(tool, args, timeout)
        done.append((tool, args, round(time.time() - tc, 1), run.s.text(r)[:300] if isinstance(r, dict) else r))
        if watch.over: what = f'MEM {watch.over // 1024} MB'; break
        if r in ('HANG', 'EOF'): what = r; break
    hits, run.pos, new = sanitizer_lines(errpath, run.pos)
    if hits and not what: what = 'SANITIZER'
    stats[ext] = stats.get(ext, 0) + 1
    if what:
        events += 1
        c = os.path.join(crashes, f'{what.split()[0].lower()}-{seed}-{k}')
        os.makedirs(c, exist_ok=True)
        shutil.copy(name, c); open(os.path.join(c, 'source.txt'), 'w').write(src + '\n' + '\n'.join(did) + '\n')
        json.dump(done, open(os.path.join(c, 'calls.json'), 'w'), indent=1)
        open(os.path.join(c, 'stderr.txt'), 'w').write(new[-200000:])
        print(f'[{k}] {what} {os.path.basename(src)} {did} at {done[-1][0]}', file=log, flush=True)
        run.s.kill(); time.sleep(1)
        run.start()
    elif k % 20 == 0:
        print(f'[{k}] {int(time.time() - t0)} s, {events} events, {stats}', file=log, flush=True)
run.s.kill()
print(f'done: {k} cases, {events} events, {stats}', file=log, flush=True)
print(f'done: {k} cases, {events} events, {stats}')
