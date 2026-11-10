# Damaged files of every kind opened in the real program (`qucs-s
# --mcp-server`, offscreen): each seed mutated (mutate.py; an archive's -
# .pptx, .xlsx, .zip - inside it, one member at a time, as often as its
# bytes), opened, read by the tools of its kind, drawn (screenshot), at
# times closed at once while it loads, then closed. A data file is imported
# beside a schematic, a stack file into an RF 2.5D model, a .lyp put beside
# a layout. A crash (EOF), a hang or a sanitizer's report is kept with the
# file and the calls.
#   QUCS=<qucs-s> python3 fuzz_docs.py SEEDS OUT SEED SECONDS [EXT,...]
import io, json, os, random, shutil, sys, time, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mutate import mutate
from mcp import Server, sanitizer_lines

APP = os.environ['QUCS']
seeds_dir, out, seed, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
only = tuple('.' + e.lstrip('.') for e in sys.argv[5].split(',')) if len(sys.argv) > 5 else None
rnd = random.Random(seed)
H = os.path.join(out, f'run-{seed}')
shutil.rmtree(H, ignore_errors=True); os.makedirs(H)
errpath = os.path.join(H, 'stderr.txt')
crashes = os.path.join(out, 'crashes'); os.makedirs(crashes, exist_ok=True)
log = open(os.path.join(H, 'log.txt'), 'w')

seeds = sorted(os.path.join(seeds_dir, f) for f in os.listdir(seeds_dir) if not f.startswith('.'))
ARCHIVES = ('.pptx', '.xlsx', '.zip')
XMLISH = ('.lyp', '.xml', '.svg', '.qrf25', '.qfem', '.qfem3d', '.dpl', '.sch')
DATA = ('.s1p', '.s2p', '.s4p', '.csv', '.ldf', '.dat', '.xlsx', '.npy', '.txt')
STACK = ('.proc', '.xml')
LAYOUT = ('.gds', '.oas')
openable = [s for s in seeds if not s.endswith(('.lyp',) + STACK + DATA) or s.endswith(('.csv', '.xlsx'))]
if only: openable = [s for s in seeds if s.endswith(only)]


def mutate_file(path):
    data = open(path, 'rb').read()
    if path.endswith(ARCHIVES) and rnd.random() < 0.7:
        try:
            zin = zipfile.ZipFile(io.BytesIO(data))
            names = zin.namelist()
            victim = rnd.choice(names)
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
                for n in names:
                    d = zin.read(n)
                    if n == victim or rnd.random() < 0.05: d = mutate(d, rnd, xml=True)
                    if rnd.random() < 0.02: continue   # a member left out
                    z.writestr(n, d)
            return buf.getvalue()
        except Exception:
            pass
    return mutate(data, rnd, xml=path.endswith(XMLISH))


def follow_ups(ext, name):
    """the tools of a document's kind, with their arguments"""
    t = []
    if ext in ('.sch', '.dpl', '.sym'):
        t += [('get_schematic', {'path': name, 'format': rnd.choice(['summary', 'overview', 'text', 'json'])}),
              ('check_schematic', {'path': name}), ('get_netlist', {'path': name}),
              ('export_image', {'path': name, 'save_as': 'fz.png'})]
    elif ext == '.qrf25':
        t += [('rf25_stack_export', {'path': name, 'file': os.path.join(os.path.dirname(name), 'out.' + rnd.choice(['xml', 'proc']))}),
              ('rf25_hfss_export', {'path': name, 'file': os.path.join(os.path.dirname(name), 'out.py')}),
              ('rf25_circuit', {'path': name}), ('fem_describe', {'path': name, 'what': 'format'})]
    elif ext in ('.qfem', '.qfem3d'):
        t += [('fem_describe', {'path': name, 'what': rnd.choice(['model', 'geometry', 'variables', 'format'])}),
              ('fem_build', {'path': name, 'what': 'geometry', 'timeout': 20}),
              ('fem_build', {'path': name, 'what': 'mesh', 'timeout': 20})]
    elif ext in LAYOUT:
        t += [('get_layout', {'path': name, 'depth': rnd.choice([0, 3, 50])}), ('find_shapes', {'path': name}),
              ('show_layout', {'path': name}), ('export_image', {'path': name, 'save_as': 'fz.png'})]
    elif ext == '.pdf':
        t += [('read_pdf', {'path': name, 'pages': 'all'}), ('find_plots', {'path': name}),
              ('read_pdf', {'path': name, 'image': True})]
    elif ext == '.pptx':
        t += [('read_pptx', {'path': name}), ('read_pptx', {'path': name, 'image': True, 'slides': [1, 2]}),
              ('show_pptx', {'path': name, 'slide': rnd.randint(1, 4)})]
    else:
        t += [('get_text', {'path': name})]
    rnd.shuffle(t)
    return t[:rnd.randint(1, len(t))]


class Run:
    def __init__(self):
        self.start()

    def start(self):
        self.s = Server(APP, H, errpath)
        self.pos = os.path.getsize(errpath)
        # a schematic with a file, for the imports; an RF 2.5D model, for the stacks
        self.host = os.path.join(self.s.ws, 'host.sch')
        shutil.copy(os.path.join(seeds_dir, 'singleOPV.sch') if os.path.exists(os.path.join(seeds_dir, 'singleOPV.sch'))
                    else [s for s in seeds if s.endswith('.sch')][0], self.host)
        self.s.call('open_document', {'path': self.host})
        self.model = None
        qrf = [s for s in seeds if s.endswith('.qrf25')]
        if qrf:
            self.model = os.path.join(self.s.ws, 'host.qrf25')
            shutil.copy(qrf[0], self.model)
            self.s.call('open_document', {'path': self.model}, timeout=120)


run = Run()
t0 = time.time(); k = 0; events = 0; stats = {}
while time.time() - t0 < seconds:
    k += 1
    calls = []
    src = rnd.choice(openable if rnd.random() < 0.75 else seeds)
    ext = os.path.splitext(src)[1].lower()
    data = mutate_file(src)
    d = os.path.join(run.s.ws, f'c{k % 7}')
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    name = os.path.join(d, 'fz' + ext)
    open(name, 'wb').write(data)
    extra = None
    if ext == '.qrf25':   # the layout it names
        lay = [s for s in seeds if s.endswith('.gds')][0]
        open(os.path.join(d, 'heater.gds'), 'wb').write(mutate(open(lay, 'rb').read(), rnd) if rnd.random() < 0.3 else open(lay, 'rb').read())
    if ext in DATA and src not in openable:
        calls = [('import_data', {'file': name, 'path': run.host, 'name': 'fz'}), ('import_data', {'remove': 'fz', 'path': run.host})]
    elif ext in STACK:
        if run.model is None: continue
        calls = [('rf25_stack_import', {'file': name, 'path': run.model}), ('fem_describe', {'path': run.model}),
                 ('undo', {'path': run.model})]
        if ext == '.xml' and rnd.random() < 0.3: calls = [('rf25_hfss_import', {'path': name, 'replace': True})]
    elif ext == '.lyp':
        lay = rnd.choice([s for s in seeds if s.endswith(LAYOUT)])
        extra = os.path.join(d, 'fz' + os.path.splitext(lay)[1])
        shutil.copy(lay, extra)
        os.rename(name, os.path.join(d, 'fz.lyp'))
        calls = [('open_document', {'path': extra}), ('show_layout', {'path': extra}),
                 ('export_image', {'path': extra, 'save_as': 'fz.png'}), ('close_document', {'path': extra, 'unsaved': 'discard'})]
    else:
        calls = [('open_document', {'path': name})]
        if rnd.random() < 0.15:
            calls.append(('close_document', {'path': name, 'unsaved': 'discard'}))   # closed while it loads
        else:
            calls += follow_ups(ext, name)
            calls.append(('screenshot', {}))
            calls.append(('close_document', {'path': name, 'unsaved': 'discard'}))
    bad = None; done = []
    for tool, args in calls:
        timeout = 120 if tool in ('fem_build', 'open_document', 'rf25_hfss_import') else 60
        r = run.s.call(tool, args, timeout=timeout)
        hits, run.pos, _ = sanitizer_lines(errpath, run.pos)
        txt = run.s.text(r)
        done.append((tool, args, txt[:300]))
        if isinstance(r, dict) and 'result' in r:
            st = stats.setdefault(f'{ext} {tool}', [0, 0]); st[1 if r['result'].get('isError') else 0] += 1
        if r in ('EOF', 'HANG'): bad = r
        elif hits: bad = 'SANITIZER'
        if bad: break
    log.write(json.dumps({'case': k, 'src': os.path.basename(src), 'calls': [(c[0], c[2][:120]) for c in done]}) + '\n'); log.flush()
    if bad:
        events += 1
        tag = f'{bad.lower()}-{seed}-{k}'
        base = os.path.join(crashes, tag)
        os.makedirs(base, exist_ok=True)
        for f in os.listdir(d): shutil.copy(os.path.join(d, f), base)
        time.sleep(1)
        _, _, tail = sanitizer_lines(errpath, max(0, run.pos - 200000))
        json.dump({'seed_file': src, 'calls': done}, open(os.path.join(base, 'calls.json'), 'w'), indent=1, default=str)
        open(os.path.join(base, 'stderr.txt'), 'w').write(tail[-60000:])
        print(f'[{seed}] case {k}: {bad} {os.path.basename(src)} after {done[-1][0]} -> {base}', flush=True)
        if bad in ('EOF', 'HANG'):
            run.s.kill(); run.start()
        else:
            # a sanitizer's report that did not stop it: go on, but from a fresh program
            run.s.kill(); run.start()
summary = f'[{seed}] {k} files in {time.time() - t0:.0f} s, {events} events'
print(summary, flush=True); log.write(summary + '\n')
json.dump(stats, open(os.path.join(H, 'stats.json'), 'w'), indent=1)
run.s.kill()
