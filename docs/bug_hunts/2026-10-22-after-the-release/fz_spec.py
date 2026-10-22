"""Fuzz the quantum spectrum's options (the Miller round's and before) on the ASan app: random wells,
continua, bulk excitons, Urbach, held states, local index windows, fields and sweeps. Each case a fresh
--mcp-server; logs sanitizer reports, crashes, hangs, non-finite globals."""
import json, os, random, sys, time, subprocess, threading
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
H = S + '/hunt2'
APP = '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
seed, count = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
def pick(values):
    # (The first two plausible, the rest hostile: hostile one time in five.)
    return rnd.choice(values[:2]) if len(values) <= 2 or rnd.random() < 0.8 else rnd.choice(values[2:])

def structure():
    kind = rnd.choice(['both', 'both', 'wellfirst', 'welllast', 'slab', 'two', 'thin'])
    W, B = pick(['9.4', '5', '20', '1']), pick(['8', '3', '15', '0.5'])
    if kind == 'both': layers = [('b', B), ('w', W), ('b', B)]
    elif kind == 'wellfirst': layers = [('w', W), ('b', B)]
    elif kind == 'welllast': layers = [('b', B), ('w', W)]
    elif kind == 'slab': layers = [('w', W)]
    elif kind == 'two': layers = [('b', B), ('w', W), ('b', '2'), ('w', W), ('b', B)]
    else: layers = [('b', '0.3'), ('w', W), ('b', '0.3')]
    geo, y, names = [], 0.0, {'w': [], 'b': []}
    for i, (k, t) in enumerate(layers):
        n = '%s%d' % (k, i)
        geo.append({"type": "rectangle", "name": n, "position": ["0", "%g" % y], "size": ["10", t]}); y += float(t); names[k].append(n)
    mats = [{"material": "well", "compound": "GaAs", "x": "0", "selection": {"only": names['w']}}]
    if names['b']: mats.append({"material": "barrier", "compound": "AlGaAs", "x": pick(["0.3", "0.1", "1"]), "selection": {"only": names['b']}})
    return kind, geo, mats

def case(i):
    kind, geo, mats = structure()
    params = [{"name": "F", "expression": "0[V/m]"}, {"name": "ex", "expression": "1"}, {"name": "c", "expression": "1"}]
    ph = {"type": "quantum", "tag": "qw", "h1d": pick(["0.5[nm]", "0.25[nm]"])}
    if rnd.random() < 0.6: ph["F"] = "F"
    if rnd.random() < 0.15: ph["ends"] = "periodic"
    if rnd.random() < 0.25: ph["valence"] = "kp4"
    sp = {"type": "spectrum", "energies": pick(["linspace(1.3[eV], 2.0[eV], 24)", "linspace(1.38[eV], 1.6[eV], 12)", "1.45[eV]", "linspace(0.5[eV], 3[eV], 20)", "1/0", "-1[eV]", "linspace(1.4[eV], 1.4[eV], 3)"]),
          "states": rnd.randint(1, 10), "linewidth": pick(["5[meV]", "2[meV]", "0[meV]", "1e-6[meV]", "1[eV]", "-1[meV]"]),
          "lineshape": rnd.choice(["gaussian", "lorentzian", "sech"]), "continuum": rnd.choice(["states", "bound"])}
    if "valence" in ph: sp["kpoints"] = 11
    for key, values in [("bulkexcitons", [True, "ex", False, 1, "1/0", "ex > 0", "maybe"]), ("urbach", ["6[meV]", "10[meV]", "0[meV]", "-1[meV]", "1[eV]", "1/0"]),
                        ("linewidthslope", ["0.05", "0", "-1", "1e6"]), ("pmin", ["0.6", "0.9", "0", "1", "2", "-1"]), ("select", ["localized", "states"]),
                        ("dnwindow", [["1.38[eV]", "1.5[eV]"], ["1.4[eV]", "1.6[eV]"], ["1.5[eV]", "1.38[eV]"], ["0", "0"], ["1/0", "2[eV]"], ["850[nm]", "880[nm]"], ["a", "b"], "1.4, 1.5", ["1.4[eV]"], ["F", "F"]]),
                        ("conservearea", [True, "c", False, "maybe", "c > 0"]), ("excitons", [True, "ex", False]), ("excitonmodel", ["coupled", "bastard"]),
                        ("fade", ["0.1[eV]", "0.05[eV]", "0[eV]", "-1[eV]", "1e3[eV]"]), ("normalize", ["stack", "length", "wells"]), ("normlength", ["10[nm]", "0[nm]"]),
                        ("excitonsize", [100, 50, 400]), ("excitonpairs", [1, 2, -3, 1000])]:
        if rnd.random() < 0.3: sp[key] = pick(values)
    if sp.get("excitonmodel") == "coupled" and sp.get("excitons"): sp["excitonsize"] = min(int(sp.get("excitonsize", 100)), 200)
    steps = [sp]
    if rnd.random() < 0.6:
        sw = {"type": "sweep", "parameters": [["F", pick(["0, 1e7", "0, 3e7, 1e7", "1e7, 0", "0, 1e9", "0, 0"]), "V/m"]]}
        if rnd.random() < 0.3: sw["parameters"].append(["ex", "0, 1", ""])
        if rnd.random() < 0.2: sw["parallel"] = 2
        steps = [sw, sp]
    return {"format": "qucs-s multiphysics 1", "parameters": params,
            "components": [{"unit": "nm", "geometry": geo, "materials": mats, "physics": [ph], "mesh": {"size": "normal", "custom": True, "hmax": "2"}}],
            "studies": [{"type": "study", "tag": "s", "steps": steps}]}

log = open(H + '/fz_spec_%d.log' % seed, 'a')
for i in range(count):
    m = case(i)
    tag = 'fz_%d_%d' % (seed, i)
    path = H + '/fz/' + tag + '.qfem'
    os.makedirs(H + '/fz', exist_ok=True)
    json.dump(m, open(path, 'w'), indent=1)
    errf = H + '/fz/' + tag + '.err'
    os.environ['MCP_STDERR'] = errf
    t = time.time()
    verdict, detail = 'ok', ''
    c = Client(H + '/fz/w', app=APP, env_extra={'ASAN_OPTIONS': 'detect_leaks=0:detect_stack_use_after_return=1', 'UBSAN_OPTIONS': 'print_stacktrace=1'})
    res = {}
    def go():
        try:
            c.call('fem_model', {'action': 'open', 'path': path})
            res['solve'] = c.call('fem_solve', {'path': path, 'study': 's', 'timeout': 600})
            if not res['solve'][0]:
                res['eval'] = c.call('fem_evaluate', {'path': path, 'study': 's', 'expression': 'qw.alphaTE', 'all': True})
        except Exception as e:
            res['exc'] = repr(e)
    th = threading.Thread(target=go); th.start(); th.join(700)
    if th.is_alive():
        verdict = 'HANG'; c.p.kill(); th.join(10)
    else:
        c.close()
    err = open(errf, errors='replace').read() if os.path.exists(errf) else ''
    if 'ERROR: AddressSanitizer' in err or 'runtime error:' in err: verdict = 'SANITIZER'; detail = [l for l in err.splitlines() if 'ERROR:' in l or 'runtime error' in l][:2]
    elif 'ASSERT' in err or 'Q_ASSERT' in err: verdict = 'ASSERT'; detail = [l for l in err.splitlines() if 'ASSERT' in l][:2]
    elif verdict == 'ok' and ('solve' not in res or res.get('exc')): verdict = 'CRASH?'; detail = res.get('exc', 'no answer')
    elif verdict == 'ok':
        e, text = res['solve']
        if e: verdict, detail = 'refused', text[:160]
        else:
            ev = res.get('eval', (True, ''))[1]
            low = (text + ev).lower()
            bad = [w for w in ('"nan', 'nan ', ' inf', '"inf', '-inf') if w in low]
            if bad: verdict, detail = 'NONFINITE', bad
    log.write('%s %s %.0fs %s %s\n' % (tag, verdict, time.time() - t, json.dumps(m['studies'][0]['steps'])[:400], detail))
    log.flush()
print('done')
