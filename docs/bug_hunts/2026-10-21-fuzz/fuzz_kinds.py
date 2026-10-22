# Fuzzes multiphysics models made from the node kinds (kinds.json, dumped by
# test_fem_engine dumpKinds) through `qucs-s --mcp-server` (the ASan/UBSan
# build): semiconductor devices (a bar, a diode, a MOS structure, a
# heterojunction) with random features of the semiconductor's kinds and
# random properties - choices, flags, expressions plausible and hostile,
# selections -, the physics' own (T, TL, Bz, noise, heterojunctions), random
# studies; process flows of random steps. Each opened, built, solved (its
# own time limit), evaluated and closed. A crash (EOF), a call past its time
# limit (HANG) or a sanitizer report is a finding: the model and the calls
# kept in findings/.
#   QUCS=<qucs-s> HUNT_RUNS=<scratch> KINDS_JSON=<kinds.json> python3 fuzz_kinds.py KIND SEED COUNT   (KIND: semi or process)
import json, os, random, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
kind_, seed_, count_ = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
# fuzz_qfem.py's server and sanitizer reading, its loop left out.
sys.argv = ['fuzz_qfem.py', str(seed_), '0']
exec(compile(open(os.path.join(HERE, '..', '2026-10-16-fuzz', 'fuzz_qfem.py')).read().split('examples = sorted(')[0], 'fuzz_qfem_head', 'exec'))
# (The kinds as test_fem_engine dumpKinds writes them: QUCS_KINDS_DUMP=kinds.json.)
KINDS = {k['type']: k for k in json.load(open(os.environ.get('KINDS_JSON', os.path.join(HERE, 'kinds.json'))))}
rnd = random.Random(seed_ * 7 + (1 if kind_ == 'semi' else 2))
log = open(os.path.join(H, f'kinds-{kind_}-{seed_}.log'), 'w')

HOSTILE_EXPR = ['0', '-1', '1e300', '-1e300', '1e-300', 'nan', '1/0', '0/0', 'x', 'y', 'T', 'x*1e6', '(', 'nosuch', '1e9[m]',
                '-1e-9[m]', 'sqrt(-1)', 'if(x>0,1,-1)', '1e-30', '2147483648', '']

# A model plausible (most of its values valid, so a flow or a device goes
# deep before anything refuses it) or hostile (each value at risk).
PLAUSIBLE = False

def scaled(default):
    d = str(default)
    if PLAUSIBLE: return rnd.choice([d, d, d, f'2*({d})', f'({d})/2'])
    return rnd.choice([d, f'2*({d})', f'({d})/100', f'({d})*1e3', f'-({d})', f'({d})*(1+x*0)'])

def value_of(p, names):
    k, d = p['kind'], p['default']
    hostile = rnd.random() < (0.005 if PLAUSIBLE else 0.05)
    if k == 0:   # Expression
        if hostile: return rnd.choice(HOSTILE_EXPR)
        return scaled(d) if d not in (None, '') else rnd.choice(['1', '0', '1e-6', '300'])
    if k == 1:   # Pair
        if hostile: return [rnd.choice(HOSTILE_EXPR), rnd.choice(HOSTILE_EXPR)]
        return d if isinstance(d, list) and rnd.random() < 0.5 else [str(rnd.uniform(-1, 3)), str(rnd.uniform(0.01, 3))]
    if k == 2:   # Text
        if PLAUSIBLE and isinstance(d, str): return d
        return rnd.choice(['', 'a', 'flow', 'x', d if isinstance(d, str) else '', 'Ω' * 50])
    if k == 3:   # Choice
        if p['choices'] and not hostile: return rnd.choice(p['choices'])
        return rnd.choice(['nosuch', '', d if isinstance(d, str) else 'x'])
    if k == 4: return rnd.random() < 0.5
    if k == 5:
        if PLAUSIBLE: return rnd.choice([1, 2, 3, 7] + ([d] if isinstance(d, int) else []))
        return rnd.choice([0, 1, 2, 3, -1, 7, 100, 1000000, 2**31 - 1])
    if k == 6: return selection(names)
    if k == 7: return rnd.sample(names, min(len(names), rnd.randint(0, 2)))
    if k == 8: return [[str(rnd.uniform(-1, 2)), str(rnd.uniform(-1, 1))] for _ in range(rnd.randint(0, 6))]
    return d

def selection(names):
    c = rnd.random()
    if c < 0.5: return {'all': True}
    if c < 0.75: return {'only': rnd.sample(names, rnd.randint(1, len(names)))}
    if c < 0.95: return {'of': [rnd.choice(names)], 'side': rnd.choice(['left', 'right', 'top', 'bottom'])}
    return {'numbers': [rnd.randint(-1, 8)]}

def feature(type_, names, short=True, share=0.15):
    k = KINDS[type_]
    f = {'type': type_.split('/', 1)[1] if short and '/' in type_ else type_}
    for p in k['properties']:
        if p['key'] == 'selection':
            f['selection'] = selection(names) if rnd.random() < 0.1 else {'all': True}
        elif rnd.random() < share:
            f[p['key']] = value_of(p, names)
    return f

# Semiconductor devices: geometry, materials and the doping, then random features.
BASES = {
    'bar': ([{'type': 'rectangle', 'label': 'bar', 'size': ['2', '0.2']}], [{'material': 'Silicon', 'selection': {'all': True}}],
            [{'type': 'doping', 'dopant': 'donor', 'N0': '1e17[1/cm^3]', 'selection': {'all': True}},
             {'type': 'ohmic', 'name': 'a', 'V0': 'Va', 'selection': {'of': ['bar'], 'side': 'left'}},
             {'type': 'ohmic', 'name': 'k', 'V0': '0[V]', 'selection': {'of': ['bar'], 'side': 'right'}}], ['bar']),
    'diode': ([{'type': 'rectangle', 'label': 'pside', 'position': ['-0.5', '0'], 'size': ['0.5', '0.2']},
               {'type': 'rectangle', 'label': 'nside', 'position': ['0', '0'], 'size': ['2', '0.2']}],
              [{'material': 'Silicon', 'selection': {'all': True}}],
              [{'type': 'doping', 'dopant': 'acceptor', 'N0': '1e19[1/cm^3]', 'selection': {'only': ['pside']}},
               {'type': 'doping', 'dopant': 'donor', 'N0': '1e16[1/cm^3]', 'selection': {'only': ['nside']}},
               {'type': 'ohmic', 'name': 'a', 'V0': 'Va', 'selection': {'of': ['pside'], 'side': 'left'}},
               {'type': 'ohmic', 'name': 'k', 'V0': '0[V]', 'selection': {'of': ['nside'], 'side': 'right'}}], ['pside', 'nside']),
    'mos': ([{'type': 'rectangle', 'label': 'body', 'position': ['0', '-0.3'], 'size': ['0.4', '0.3']},
             {'type': 'rectangle', 'label': 'oxide', 'position': ['0', '0'], 'size': ['0.4', '0.01']}],
            [{'material': 'Silicon', 'selection': {'only': ['body']}}, {'material': 'Silicon dioxide', 'selection': {'only': ['oxide']}}],
            [{'type': 'doping', 'dopant': 'acceptor', 'N0': '1e17[1/cm^3]', 'selection': {'only': ['body']}},
             {'type': 'gate', 'name': 'g', 'V0': 'Va', 'phim': '4.05[eV]', 'selection': {'of': ['oxide'], 'side': 'top'}},
             {'type': 'ohmic', 'name': 'b', 'V0': '0[V]', 'selection': {'of': ['body'], 'side': 'bottom'}}], ['body', 'oxide']),
    'hetero': ([{'type': 'rectangle', 'label': 'gaas', 'position': ['-0.3', '0'], 'size': ['0.3', '0.1']},
                {'type': 'rectangle', 'label': 'algaas', 'position': ['0', '0'], 'size': ['0.3', '0.1']}],
               [{'material': 'Gallium arsenide', 'selection': {'only': ['gaas']}}, {'material': 'Aluminium gallium arsenide', 'selection': {'only': ['algaas']}}],
               [{'type': 'doping', 'dopant': 'donor', 'N0': '1e17[1/cm^3]', 'selection': {'all': True}},
                {'type': 'ohmic', 'name': 'a', 'V0': 'Va', 'selection': {'of': ['gaas'], 'side': 'left'}},
                {'type': 'ohmic', 'name': 'k', 'V0': '0[V]', 'selection': {'of': ['algaas'], 'side': 'right'}}], ['gaas', 'algaas']),
}
SEMI_FEATURES = [t for t in KINDS if t.startswith('semiconductor/') and t not in ('semiconductor/insulation',)]
def pick(good, bad):
    return rnd.choice(good if PLAUSIBLE else good + bad)

STEPS = [lambda: [{'type': 'stationary'}],
         lambda: [{'type': 'sweep', 'parameters': [['Va', pick(['-1, 0, 0.5', '0, 0.3, 0.6', '0.1'], ['-5, -10', '0, 1e3', 'nan']), 'V']]}, {'type': 'stationary'}],
         lambda: [{'type': 'stationary'}, {'type': 'frequency', 'frequencies': pick(['1[MHz]', '1[kHz], 1[GHz]'], ['0', '-1[Hz]', '1e30[Hz]'])}],
         lambda: [{'type': 'stationary'}, {'type': 'transient', 'times': pick(['range(0, 10[ps], 100[ps])', 'range(0, 1[ns], 5[ns])'], ['range(0, 0, 1)', '0']), 'rtol': '1e-4'}]]

def semi_model():
    base = rnd.choice(list(BASES))
    geom, mats, feats, names = BASES[base]
    features = json.loads(json.dumps(feats))
    for _ in range(rnd.randint(1, 6)):
        t = rnd.choice(SEMI_FEATURES)
        if t in ('semiconductor/ohmic', 'semiconductor/schottky', 'semiconductor/gate') and rnd.random() < 0.7: continue
        # (Plausible: what fits the device - tunnelling through the MOS's
        # oxide only, no nitrides' polarization, no light without a mode
        # analysis, no lasing without a cavity.)
        if PLAUSIBLE and (t in ('semiconductor/polarization', 'semiconductor/optical', 'semiconductor/stimulated', 'semiconductor/laser')
                          or (t == 'semiconductor/fn' and base != 'mos')): continue
        if t == 'semiconductor/quantum' and rnd.random() < 0.9: continue
        features.append(feature(t, names))
    # (A contact's circuit, at times.)
    for f in features:
        if f.get('type') in ('ohmic', 'schottky') and rnd.random() < 0.2:
            f.update({'circuit': rnd.choice(['series', 'biastee', 'none']), 'Rs': rnd.choice(['50[Ohm]', '0'] + ([] if PLAUSIBLE else ['-1[Ohm]', '1e12[Ohm]'])),
                      'RL': rnd.choice(['50[Ohm]', '0', '1e6[Ohm]']), 'Cp': rnd.choice(['0', '1[pF]'] + ([] if PLAUSIBLE else ['-1[pF]']))})
    physics = {'type': 'semiconductor', 'tag': 'semi', 'features': features}
    for p in KINDS['semiconductor']['properties']:
        if p['key'] in ('selection',): continue
        if rnd.random() < (0.03 if p['key'] in ('space', 'axis', 'at') else 0.12): physics[p['key']] = value_of(p, names)
    mesh = {'size': 'normal', 'custom': True, 'hmax': rnd.choice(['0.05', '0.1', '0.02'] if PLAUSIBLE else ['0.05', '0.1', '0.02', '1e-4', '10', '0'])}
    return {'format': 'qucs-s multiphysics 1', 'parameters': [{'name': 'Va', 'expression': rnd.choice(['0[V]', '0.5[V]', '-2[V]', '1[V]'])}],
            'components': [{'unit': 'um', 'geometry': geom, 'materials': mats, 'physics': [physics], 'mesh': mesh}],
            'studies': [{'type': 'study', 'steps': rnd.choice(STEPS)()}]}

PROCESS_STEPS = [t for t in KINDS if t.startswith('process/')]
MATERIALS = ['Silicon', 'Silicon dioxide', 'Silicon nitride', 'Aluminium', 'Gallium arsenide', 'Photoresist', 'Indium phosphide', 'nosuch', 'Aluminium gallium arsenide', 'Germanium']

def process_model():
    steps = []
    names = ['substrate', 'oxide', 'film', 'etch', 'nosuch']
    real = MATERIALS[:7] + MATERIALS[8:]
    # (A flow starts with a substrate or a stack, mostly.)
    if PLAUSIBLE or rnd.random() < 0.6:
        steps.append({'type': 'process/substrate', 'material': rnd.choice(['Silicon'] * 4 + (real if PLAUSIBLE else MATERIALS)),
                      'thickness': rnd.choice(['2[um]', '0.5[um]', '5[um]'] if PLAUSIBLE else ['2[um]', '0.5[um]', '0', '-1[um]', '1e3[um]']),
                      'orientation': rnd.choice(['100', '110', '111'])})
    elif rnd.random() < 0.9:
        steps.append({'type': 'process/stack', 'layers': [[f'l{k}', rnd.choice(MATERIALS[:7]), rnd.choice(['0.5', '0.1', '0.01', '0', '2'])] for k in range(rnd.randint(1, 4))]})
    for _ in range(rnd.randint(1, 8)):
        t = rnd.choice(PROCESS_STEPS)
        # (Plausible: a substrate or a stack first only, no intermixing
        # without a III-V, no expression left empty, a mask of openings or
        # none - not a layout's layer -, an etch's depth, a CMP's height.)
        if PLAUSIBLE and t in ('process/substrate', 'process/stack', 'process/intermix'): continue
        f = feature(t, names, short=False, share=0.2 if PLAUSIBLE else 0.45)
        f['type'] = t
        keys = [p['key'] for p in KINDS[t]['properties']]
        if PLAUSIBLE:
            for p in KINDS[t]['properties']:
                if p['kind'] == 0 and p['default'] in ('', None): f.pop(p['key'], None)
            if 'mask' in keys: f['mask'] = rnd.choice(['none', 'none', 'openings'])
            if t == 'process/etch': f['depth'] = rnd.choice(['0.2', '0.5', '0.05'])
            if t == 'process/planarize': f['height'] = rnd.choice(['1', '0.5', '3'])
        if 'material' in keys and rnd.random() < 0.8: f['material'] = rnd.choice(real if PLAUSIBLE else MATERIALS)
        if (f.get('mask') == 'openings' if PLAUSIBLE else rnd.random() < 0.4) and 'openings' in keys:
            f['mask'] = 'openings'
            f['openings'] = rnd.choice(['2, 4', '1, 2; 6, 8', '4, 6'] if PLAUSIBLE else ['2, 4', '0, 10', '5, 5', '-1, 12', 'nosuch', '1, 2; 6, 8', '4.9, 5.1'])
        steps.append(f)
    if PLAUSIBLE:
        flow = {'type': 'process', 'tag': 'pf1', 'label': 'flow', 'width': rnd.choice(['10', '4', '1']), 'dx': rnd.choice(['10[nm]', '20[nm]', '']),
                'dy': rnd.choice(['2[nm]', '5[nm]']), 'depth': rnd.choice(['0.6', '1', '2']), 'features': steps}
    else:
        flow = {'type': 'process', 'tag': 'pf1', 'label': 'flow', 'width': rnd.choice(['10', '10', '10', '1', '0', '-1', '1e-6']),
                'dx': rnd.choice(['2.5[nm]', '10[nm]', '1[nm]', '0']), 'dy': rnd.choice(['2.5[nm]', '1[nm]', '0.1[nm]']),
                'depth': rnd.choice(['0.6', '1', '0', '100']), 'features': steps}
    return {'format': 'qucs-s multiphysics 1',
            'components': [{'unit': 'um', 'geometry': [flow], 'materials': [{'material': 'Silicon', 'selection': {'all': True}}],
                            'mesh': {'size': 'normal'}}], 'studies': []}

s = Server(); pos = os.path.getsize(errpath); findings = 0; t0 = time.time(); stats = {}
EVAL = ['semi.I_a', 'semi.I_k', 'semi.I_g', 'max(semi.n)', 'min(semi.mun)', 'max(semi.Jx)', 'max(semi.Tn)', 'max(semi.H)', 'semi.Sn_a', '1/0']
refusals = {}
for i in range(count_):
    PLAUSIBLE = rnd.random() < 0.6
    model = semi_model() if kind_ == 'semi' else process_model()
    text = json.dumps(model, indent=1)
    path = os.path.join(WS, f'k{i % 20}.qfem')
    open(path, 'w', encoding='utf-8').write(text)
    calls = [('open_document', {'path': path}, 120), ('fem_build', {'path': path, 'timeout': 30}, 150)]
    if kind_ == 'semi':
        calls += [('fem_solve', {'path': path, 'timeout': 30}, 180), ('fem_evaluate', {'path': path, 'expression': rnd.choice(EVAL)}, 120)]
    else:
        calls += [('fem_describe', {'path': path, 'what': 'geometry'}, 120)]
    calls += [('close_document', {'path': path, 'unsaved': 'discard'}, 120)]
    done, bad, hits = [], None, []
    for name_, args, limit in calls:
        r, took = s.call(name_, args, limit)
        done.append((name_, round(took, 1)))
        st = stats.setdefault(name_, [0, 0, 0.0]); st[0] += 1; st[2] = max(st[2], took)
        if isinstance(r, dict) and r.get('result', {}).get('isError'):
            st[1] += 1
            # (Why, each call's first words: what the generator should make valid.)
            # (Its own name: the model's text, kept for a slow call or a finding, was this refusal's - the hunt of 22 October, N12.)
            said = (r['result'].get('content') or [{}])[0].get('text', '')
            key = f"{name_} {'plausible' if PLAUSIBLE else 'hostile'}: {said[:110]}"
            refusals[key] = refusals.get(key, 0) + 1
        if took > 20:
            keep = os.path.join(FIND, f'slow-{kind_}-{seed_}-{i}-{name_}')
            os.makedirs(keep, exist_ok=True)
            open(os.path.join(keep, 'model.qfem'), 'w', encoding='utf-8').write(text)
            json.dump({'slow': name_, 'took': took, 'calls': done}, open(os.path.join(keep, 'slow.json'), 'w'), indent=1, default=str)
        hits, pos = sanitizer(pos)
        if r in ('EOF', 'HANG'): bad = f'{r} in {name_}'
        elif hits: bad = 'SANITIZER ' + hits[0][:200]
        if bad: break
    if bad:
        findings += 1
        keep = os.path.join(FIND, f'{kind_}-{seed_}-{i}')
        os.makedirs(keep, exist_ok=True)
        open(os.path.join(keep, 'model.qfem'), 'w', encoding='utf-8').write(text)
        json.dump({'what': bad, 'calls': done, 'hits': hits[:20]}, open(os.path.join(keep, 'finding.json'), 'w'), indent=1)
        print(f'[{kind_} {seed_}] model {i}: {bad}', flush=True)
        log.write(json.dumps({'model': i, 'what': bad, 'calls': done}) + '\n'); log.flush()
        s.kill(); s = Server(); pos = os.path.getsize(errpath)
    if i % 20 == 0: print(f'[{kind_} {seed_}] {i} models, {findings} findings, {time.time() - t0:.0f} s', flush=True)
print(' '.join(f'{k}:{v[0]} calls, {v[1]} refused, longest {v[2]:.1f} s' for k, v in sorted(stats.items())))
for k, v in sorted(refusals.items(), key=lambda kv: -kv[1])[:25]: print(f'  {v:4d}  {k}')
print(f'[{kind_} {seed_}] {count_} models, {findings} findings, {time.time() - t0:.0f} s', flush=True)
s.kill()
