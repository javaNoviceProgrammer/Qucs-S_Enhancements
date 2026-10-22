import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq import Q, lines
HERE = os.path.dirname(os.path.abspath(__file__))
for i, T in enumerate(['300[K]', '1/0', 'sqrt(-1)', '-5[K]', '0[K]', '1e9[K]']):
    m = json.load(open('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/multiphysics/gaas_quantum_well.qfem'))
    for c in m['components']:
        for ph in c.get('physics', []):
            if ph.get('type') == 'quantum': ph['T'] = T
    path = os.path.join(HERE, f'temp{i}.qfem'); json.dump(m, open(path, 'w'))
    q = Q(path)
    r = q.call('fem_solve', {'study': 'std2', 'timeout': 300}, 400)
    ev = q.call('fem_evaluate', {'study': 'std2', 'expression': 'qw.alphaTE'}, 120)
    v = [x.get('value') for x in ev.get('values', [])] if isinstance(ev, dict) and 'values' in ev else str(ev)[:120]
    print(f'T={T}: solve {"ERROR " + str(r.get("error"))[:150] if "error" in r else "ok"}; alphaTE at 1.85 eV {v}', flush=True)
    q.close()
