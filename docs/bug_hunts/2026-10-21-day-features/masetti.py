# Masetti's mobility with hostile constants on the shipped pn diode: the forward current at 0.7 V.
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq import Q, lines
HERE = os.path.dirname(os.path.abspath(__file__))
base = json.load(open('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/multiphysics/pn_diode.qfem'))
for i, extra in enumerate([{}, {'mmaxn': '1/0'}, {'m1n': 'sqrt(-1)'}, {'mu1n': '1/0'}, {'pcn': 'sqrt(-1)'}, {'mmaxn': '-1[cm^2/(V*s)]'}]):
    m = json.loads(json.dumps(base))
    feats = m['components'][0]['physics'][0]['features']
    feats.append(dict({'type': 'model', 'label': 'Masetti', 'mobility': 'masetti', 'selection': {'all': True}}, **extra))
    m['studies'][0]['steps'][0]['parameters'] = [['Va', '0.7', 'V']]
    m['studies'][0]['steps'] = m['studies'][0]['steps'][:2]
    path = os.path.join(HERE, f'mas{i}.qfem'); json.dump(m, open(path, 'w'))
    q = Q(path)
    r = q.call('fem_solve', {}, 600)
    err = r.get('error') if isinstance(r, dict) else None
    ev = q.call('fem_evaluate', {'expression': 'semi.I_a'}, 120)
    vals = [v.get('value') for v in ev.get('values', [])] if isinstance(ev, dict) and 'values' in ev else str(ev)[:160]
    print(f'{extra}: solve {"ERROR " + str(err)[:200] if err else "ok"}; I_a {vals}', flush=True)
    q.close()
