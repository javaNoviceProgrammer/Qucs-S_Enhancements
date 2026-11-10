# A monkey for the viewers other than the schematic's (test_gui_monkey
# walks those): a document of each kind open - a data display, a 2D and a
# 3D multiphysics model, an RF 2.5D model, a layout, a PDF, a deck, a
# Markdown file, a picture, a schematic - and raw input sent to each through
# the real program's send_input (clicks, double and right clicks, drags,
# keys, typing, on the tab's view and on its bar), its bar's controls set
# and pressed (set_ui), its menus chosen from (context_menu), its show_*
# tool given odd arguments, a build or a solve started and the tab closed
# while it works, tabs closed and opened again. A crash (EOF), a hang or a
# sanitizer's report is kept with the steps before it.
#   QUCS=<qucs-s> python3 fuzz_viewers.py SEEDS OUT SEED SECONDS
import json, os, random, shutil, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, sanitizer_lines

APP = os.environ['QUCS']
seeds_dir, out, seed, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rnd = random.Random(seed)
H = os.path.join(out, f'run-{seed}')
shutil.rmtree(H, ignore_errors=True); os.makedirs(H)
errpath = os.path.join(H, 'stderr.txt')
crashes = os.path.join(out, 'crashes'); os.makedirs(crashes, exist_ok=True)
log = open(os.path.join(H, 'steps.txt'), 'w')

DOCS = ['display.dpl', 'coax.qfem', 'pn_diode.qfem', 'tsv.qfem3d', 'model.qrf25', 'thermo_optic_heater.gds', 'klayout_sample.oas',
        'test1.pdf', 'text10.pptx', 'svg_fill.pptx', 'README.md', 'heatsink.svg', 'singleOPV.sch', 'sheet.xlsx']
KIND_TARGET = {'.sch': 'canvas', '.dpl': 'canvas', '.gds': 'layout', '.oas': 'layout', '.pptx': 'slide'}
KEYS = ['Escape', 'Return', 'Delete', 'Backspace', 'Ctrl+Z', 'Ctrl+Shift+Z', 'Ctrl+A', 'Ctrl+C', 'Ctrl+V', 'Ctrl+X', 'Ctrl+F',
        'Ctrl++', 'Ctrl+-', 'Ctrl+0', '+', '-', 'Left', 'Right', 'Up', 'Down', 'PgDown', 'PgUp', 'Home', 'End', 'Space', 'Tab',
        'Shift+Tab', 'F', 'G', 'R', 'M', 'S', 'P', 'N', 'F5', 'Escape, Escape', 'Ctrl+D', 'Ctrl+R', 'Ctrl+E', 'Ctrl+W']
UNSAFE = ('System', 'File Manager', 'Finder', 'Trash', 'Delete File', 'Terminal', 'Reveal', 'Quit', 'Print', 'Open with',
          'Browser', 'Mail', 'Share')
TEXTS = ['', 'x', '0', '-1', '1e308', 'NaN', '999999999', '3[mm]', 'a' * 3000, '‮', '%1', '../../x', '1/0', 'M1']


def P():
    return [rnd.choice([0, 1, -1, 5, 50, 200, 300, 576, 1000, -5000, 1e9]) if rnd.random() < 0.1 else rnd.uniform(0, 600),
            rnd.uniform(0, 340) if rnd.random() > 0.1 else rnd.choice([0, -1, 1e9, 331])]


class Run:
    def __init__(self):
        self.start()

    def start(self):
        self.s = Server(APP, H, errpath)
        self.pos = os.path.getsize(errpath)
        self.open = []
        for d in DOCS:
            src = os.path.join(seeds_dir, d)
            if not os.path.exists(src): continue
            shutil.copy(src, os.path.join(self.s.ws, d))
            if d.endswith('.qrf25'): shutil.copy(os.path.join(seeds_dir, 'thermo_optic_heater.gds'), os.path.join(self.s.ws, 'heater.gds'))
            if d.endswith('.dpl'): shutil.copy(os.path.join(seeds_dir, 'singleOPV.sch'), os.path.join(self.s.ws, 'singleOPV.sch'))
            self.s.call('open_document', {'path': os.path.join(self.s.ws, d)}, timeout=180)
            self.open.append(d)


def step(run):
    """one step: [(tool, args)] to call in turn"""
    if not run.open or rnd.random() < 0.04:
        d = rnd.choice(DOCS)
        return [('open_document', {'path': os.path.join(run.s.ws, d)})], d
    d = rnd.choice(run.open)
    ext = os.path.splitext(d)[1]
    calls = [('show_document', {'path': d})] if rnd.random() < 0.7 else []
    c = rnd.random()
    if c < 0.30:
        a = {'target': 'document', 'click': P()}
        if rnd.random() < 0.3: a['button'] = rnd.choice(['left', 'right', 'middle'])
        if rnd.random() < 0.2: a['double'] = True
        if rnd.random() < 0.3: a['drag_to'] = P()
        elif rnd.random() < 0.1: a['drag_through'] = [P() for _ in range(rnd.choice([2, 10, 300]))]
        if rnd.random() < 0.2: a['modifiers'] = rnd.sample(['shift', 'ctrl', 'alt', 'meta'], rnd.randint(1, 2))
        if rnd.random() < 0.4: a['keys'] = ', '.join(rnd.choice(KEYS) for _ in range(rnd.randint(1, 4)))
        if rnd.random() < 0.1: a['text'] = rnd.choice(TEXTS)
        calls.append(('send_input', a))
    elif c < 0.40 and ext in KIND_TARGET:
        a = {'target': KIND_TARGET[ext], 'path': d, 'click': P(), 'pixels': rnd.random() < 0.5}
        if rnd.random() < 0.4: a['drag_to'] = P()
        if rnd.random() < 0.3: a['keys'] = rnd.choice(KEYS)
        if ext == '.pptx' and rnd.random() < 0.5: a['slide'] = rnd.choice([1, 2, 3, 10, 0, -1, 9999])
        calls.append(('send_input', a))
    elif c < 0.58:
        calls.append(('__ui__', {'path': d}))
    elif c < 0.66:
        on = rnd.choice([{'tab': d}] + ([{'slide': rnd.randint(0, 12)}] if ext == '.pptx' else [])
                        + ([{'node': rnd.choice(['m1', 'd1', 'gnd1', 'stack', 'ports', 'mesh', 'layout', 'x'])}] if ext == '.qrf25' else [])
                        + ([{'canvas': P()}, {'diagram': rnd.randint(1, 3)}] if ext in ('.sch', '.dpl') else []))
        calls.append(('__menu__', {'on': on, 'path': d}))
    elif c < 0.76:
        if ext in ('.gds', '.oas'):
            a = {'path': d}
            for k, v in [('zoom', None), ('grid', rnd.random() < 0.5), ('rulers', rnd.random() < 0.5), ('pen', rnd.random() < 0.5),
                         ('depth', rnd.choice([0, 1, 50, -1])), ('region', [rnd.uniform(-1e4, 1e4) for _ in range(4)]),
                         ('layer_look', rnd.choice(['klayout', 'viewer', 'x'])), ('theme', rnd.choice(['dark', 'light', 'x'])),
                         ('mesh', rnd.random() < 0.3), ('texts', rnd.random() < 0.5), ('reload', rnd.random() < 0.2)]:
                if v is not None and rnd.random() < 0.4: a[k] = v
            calls.append(('show_layout', a))
        elif ext == '.pptx':
            a = {'path': d}
            for k, v in [('slide', rnd.choice([1, 2, 5, 0, 999, -3, 'x'])), ('zoom', rnd.choice([0.1, 1, 8, 0, -1, 1e9, 'fit'])),
                         ('notes', rnd.random() < 0.5), ('sidebar', rnd.random() < 0.5), ('pen', rnd.random() < 0.5),
                         ('play', rnd.random() < 0.3), ('find', rnd.choice(['', 'a', 'Slide', 'x' * 500])), ('reload', rnd.random() < 0.2),
                         ('region', [rnd.uniform(-100, 1000) for _ in range(4)])]:
                if rnd.random() < 0.4: a[k] = v
            calls.append(('show_pptx', a))
        elif ext == '.pdf':
            a = {'path': d}
            for k, v in [('page', rnd.choice([1, 2, 0, -1, 99999, 'x', '1'])), ('zoom', rnd.choice([0.01, 1, 64, 0, -2, 1e9, 'fit'])),
                         ('find', rnd.choice(['', 'a', 'the', 'x' * 500])), ('region', [rnd.uniform(-100, 1000) for _ in range(4)])]:
                if rnd.random() < 0.5: a[k] = v
            calls.append(('show_pdf', a))
        elif ext in ('.dpl', '.sch'):
            calls.append(('show_data_display', {'path': d, 'pen': rnd.random() < 0.5}))
            calls.append(('add_diagram', {'path': d, 'type': rnd.choice(['Rect', 'Polar', 'Smith', 'Tab', '3D-Cartesian', 'x']),
                                          'x': rnd.randint(-100, 900), 'y': rnd.randint(-100, 900),
                                          'traces': rnd.choice([[], ['v(out)'], ['x' * 300]])}))
        else:
            calls.append(('screenshot', {'path': d, 'scale': rnd.choice([0.1, 1, 4, 0, -1])}))
    elif c < 0.84:
        if ext in ('.qfem', '.qfem3d'):
            t = rnd.choice([('fem_build', {'path': d, 'what': 'mesh', 'timeout': 1}), ('fem_solve', {'path': d, 'timeout': 1}),
                            ('fem_build', {'path': d, 'timeout': 1})])
        elif ext == '.qrf25':
            t = rnd.choice([('rf25_solve', {'path': d, 'timeout': 1}), ('rf25_inductance', {'path': d, 'timeout': 1})])
        else:
            t = ('screenshot', {'path': d})
        calls.append(t)
        if rnd.random() < 0.6:   # closed while it works
            calls.append(('close_document', {'path': d, 'unsaved': 'discard'}))
    elif c < 0.90:
        calls.append(('close_document', {'path': d, 'unsaved': rnd.choice(['discard', 'save'])}))
    elif c < 0.95:
        calls.append(('set_ui', {'area': 'tabs', 'set': [{'control': 'tabs', 'value': rnd.choice(run.open + ['x'])}]}))
    else:
        calls.append((rnd.choice(['undo', 'redo']), {'path': d}))
    return calls, d


def expand(run, tool, args):
    """the __ui__ and __menu__ steps: read, then pick from what is there"""
    if tool == '__ui__':
        r = run.s.call('get_ui', {'area': 'document'})
        try: ctl = json.loads(run.s.text(r)).get('controls', [])
        except Exception: ctl = []
        ctl = [c for c in ctl if c.get('enabled', True) and not any(u in json.dumps(c) for u in ('System', 'File Manager'))]
        if not ctl: return []
        c = rnd.choice(ctl)
        kind = c.get('kind', '')
        if kind == 'button':
            if c.get('menu'):
                ent = [e for e in c['menu'] if not any(u in e for u in UNSAFE)]
                if not ent: return []
                return [('set_ui', {'area': 'document', 'press': f"{c['label']} > {rnd.choice(ent)}"})]
            return [('set_ui', {'area': 'document', 'press': c['id']})]
        if 'list' in kind or kind == 'tabs':
            v = rnd.choice(c.get('items', ['x']) + [rnd.choice(TEXTS)])
        elif kind == 'slider':
            v = rnd.choice([0, 1, -1, 50, 100, 1e9])
        elif kind == 'check box':
            v = rnd.random() < 0.5
        else:
            v = rnd.choice(TEXTS + ['1', '2', '10', '0.5'])
        return [('set_ui', {'area': 'document', 'set': [{'control': c['id'], 'value': v}]})]
    if tool == '__menu__':
        r = run.s.call('context_menu', {'on': args['on'], 'path': args['path']})
        try:
            m = json.loads(run.s.text(r))
            ent = [e.get('entry') or e.get('name') or e.get('text') for e in m.get('entries', m.get('menu', [])) if isinstance(e, dict) and e.get('enabled', True)]
        except Exception:
            ent = []
        ent = [e for e in ent if isinstance(e, str) and not any(u in e for u in UNSAFE)]
        if not ent: return []
        return [('context_menu', {'on': args['on'], 'path': args['path'], 'choose': rnd.choice(ent)}),
                ('get_dialog', {})]
    return [(tool, args)]


def answer_dialog(run):
    """a dialog left waiting: one of its buttons pressed (a few tries), its fields now and then given odd text first"""
    for _ in range(4):
        r = run.s.call('get_dialog', {}, timeout=60)
        try: dlg = json.loads(run.s.text(r))
        except Exception: return
        dlg = dlg.get('dialog', dlg)
        ctl = dlg.get('controls', []) if isinstance(dlg, dict) else []
        if not ctl: return
        buttons = [c.get('label') or c.get('id') for c in ctl if c.get('kind') == 'button' and c.get('enabled', True)]
        fields = [c['id'] for c in ctl if c.get('kind') == 'field']
        a = {'press': rnd.choice(buttons) if buttons and rnd.random() < 0.8 else rnd.choice(['Cancel', 'Close', 'Discard', 'No', 'OK'])}
        if fields and rnd.random() < 0.3: a['set'] = [{'control': rnd.choice(fields), 'value': rnd.choice(TEXTS)}]
        recent.append(('set_dialog', a, '')); log.write(f'  set_dialog {json.dumps(a)[:200]}\n')
        r = run.s.call('set_dialog', a, timeout=60)
        if r in ('EOF', 'HANG'): return


recent = []
run = Run()
t0 = time.time(); i = 0; events = 0; recent = []; stats = {}
while time.time() - t0 < seconds:
    i += 1
    calls, d = step(run)
    seq = []
    for tool, args in calls:
        seq += expand(run, tool, args) if tool.startswith('__') else [(tool, args)]
    bad = None
    for tool, args in seq:
        r = run.s.call(tool, args, timeout=120)
        hits, run.pos, _ = sanitizer_lines(errpath, run.pos)
        txt = run.s.text(r)
        recent.append((tool, args, txt[:200])); recent = recent[-25:]
        log.write(f'{i} {tool} {json.dumps(args, default=str)[:300]}\n    -> {txt[:200]}\n')
        if isinstance(r, dict) and 'result' in r:
            st = stats.setdefault(tool, [0, 0]); st[1 if r['result'].get('isError') else 0] += 1
        if tool == 'close_document' and isinstance(r, dict) and not r.get('result', {}).get('isError') and d in run.open: run.open.remove(d)
        if tool == 'open_document' and isinstance(r, dict) and not r.get('result', {}).get('isError') and d not in run.open: run.open.append(d)
        if 'waits for an answer' in txt or (tool == 'get_dialog' and '"controls"' in txt):
            answer_dialog(run)   # leave no dialog up
        if r in ('EOF', 'HANG'): bad = r
        elif hits: bad = 'SANITIZER'
        if bad: break
    log.flush()
    if bad:
        events += 1
        tag = f'{bad.lower()}-{seed}-{i}'
        time.sleep(1)
        _, _, tail = sanitizer_lines(errpath, max(0, run.pos - 200000))
        json.dump({'step': i, 'what': bad, 'recent': recent}, open(os.path.join(crashes, tag + '.json'), 'w'), indent=1, default=str)
        open(os.path.join(crashes, tag + '.stderr'), 'w').write(tail[-60000:])
        print(f'[{seed}] step {i}: {bad} {seq[-1][0] if seq else "?"} on {d}', flush=True)
        run.s.kill(); run.start()
print(' '.join(f'{k}:{v[0]}/{v[0]+v[1]}' for k, v in sorted(stats.items())))
print(f'[{seed}] {i} steps, {events} events, {time.time() - t0:.0f} s', flush=True)
run.s.kill()
