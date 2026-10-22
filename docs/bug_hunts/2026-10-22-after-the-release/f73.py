# a sweep's point that fails from where it starts and from the start: what is said, what is kept
import sys, json
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
from f6 import junction
m = junction("1d", [], "0.07", {"h1d": "5[nm]"})
m['parameters'].append({"name": "N", "expression": "1e17[1/cm^3]"})
m['components'][0]['physics'][0]['features'][0]['N0'] = 'N'
m['studies'][0]['steps'] = [{"type": "sweep", "parameters": [["N", "1e16, 1e17", "1/cm^3"], ["Va", "0, 0.3, " + sys.argv[1], "V"]]}, {"type": "stationary"}]
r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'semi.V', 'at': [[0.3, 0.1]], 'all': True})], tag='f73', width=0)
for e, t in r:
    try:
        j = json.loads(t)
        print('WARN', [w[:260] for w in j.get('warnings', [])]); print('LOG', [l[:200] for l in j.get('log', []) if 'start' in l or 'ail' in l][:6])
        print('VALUES', [(x['name'][:40], x['value']) for x in j.get('values', [])])
    except Exception: print('ERR' if e else 'TEXT', t[:700])
