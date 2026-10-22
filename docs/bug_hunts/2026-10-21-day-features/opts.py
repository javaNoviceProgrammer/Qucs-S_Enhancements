# The spectrum's new options with hostile values: each a copy of the quantum well example.
import sys, os, json, copy
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq import Q, lines
HERE = os.path.dirname(os.path.abspath(__file__))
base = json.load(open('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/multiphysics/gaas_quantum_well.qfem'))
def nodes(n):
    if isinstance(n, dict):
        yield n
        for v in n.values(): yield from nodes(v)
    elif isinstance(n, list):
        for v in n: yield from nodes(v)
cases = [(k, v) for k, v in [
    ('linewidth', '1/0'), ('linewidth', 'sqrt(-1)'), ('linewidth', 'inf'), ('linewidth', '0'),
    ('density', 'sqrt(-1)'), ('density', '1/0[1/m^3]'), ('fade', 'sqrt(-1)'), ('nr', '1/0'), ('epsr', '1/0'),
    ('urbach', '1/0'), ('linewidthslope', '10'), ('excitonpairs', '-3'),
]]
only = sys.argv[1:]
for i, (key, val) in enumerate(cases):
    if only and str(i) not in only: continue
    m = copy.deepcopy(base)
    for n in nodes(m):
        if n.get('kind') == 'spectrum' or n.get('type') == 'spectrum':
            props = n.setdefault('properties', n) if 'properties' in n else n
            props[key] = val
            if key in ('urbach',): props['bulkexcitons'] = True
            if key == 'excitonpairs': props['excitons'] = True; props['excitonmodel'] = 'coupled'
    path = os.path.join(HERE, f'case{i}.qfem')
    json.dump(m, open(path, 'w'), indent=1)
    q = Q(path)
    r = q.call('fem_solve', {'study': 'std2'}, 600)
    err = r.get('error') if isinstance(r, dict) else None
    ev = q.call('fem_evaluate', {'study': 'std2', 'expression': 'qw.alphaTE'} , 120)
    import re
    txt = json.dumps(ev)
    nums = re.findall(r'(-?[0-9.]+(?:e[-+]?[0-9]+)?|nan|inf|-inf|NaN|Infinity|-Infinity|null)', txt)
    bad = sum(1 for x in nums if x.lower() in ('nan','inf','-inf','infinity','-infinity','null'))
    print(f'{i} {key}={val!r}: solve {"ERROR " + str(err)[:120] if err else "ok"}; eval keys {list(ev)[:6] if isinstance(ev, dict) else type(ev)}; non-finite tokens {bad}; sample {txt[:260]}', flush=True)
    q.close()
