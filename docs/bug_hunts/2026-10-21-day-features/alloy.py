# The barrier's Al fraction out of 0..1, and NaN: the States study's levels.
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mq import Q, lines
HERE = os.path.dirname(os.path.abspath(__file__))
src = open('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/multiphysics/gaas_quantum_well.qfem').read()
for i, x in enumerate(['0.3', '1.7', '-0.3', 'sqrt(-1)', '1/0', '1e300']):
    path = os.path.join(HERE, f'alloy{i}.qfem')
    open(path, 'w').write(src.replace('"x": "0.3"', '"x": "%s"' % x))
    q = Q(path)
    r = q.call('fem_solve', {'study': 'std1'}, 600)
    err = r.get('error') if isinstance(r, dict) else None
    ev = q.call('fem_evaluate', {'study': 'std1', 'expression': 'qw.Ee1; qw.Ehh1'}, 120)
    vals = [v.get('value') for v in ev.get('values', [])] if isinstance(ev, dict) else ev
    print(f'x={x}: solve {"ERROR " + str(err)[:160] if err else "ok"}; warnings {[l[:110] for l in lines(r, "warn")][:2]}; levels {vals}', flush=True)
    q.close()
