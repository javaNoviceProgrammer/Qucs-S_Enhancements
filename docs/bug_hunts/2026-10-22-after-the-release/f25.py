import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run, H
m0 = json.load(open(H + '/fz/ph_54_57.qfem'))
for states in (1, 8, 32):
    for edge in (False, True):
        m = json.loads(json.dumps(m0)); sp = m['studies'][0]['steps'][0]; sp['states'] = states; sp['excitons'] = False
        if not edge: sp.pop('bulkexcitons', None); sp.pop('urbach', None)
        r = run(m, [('fem_solve', {'study': 's'}), ('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'all': True})], tag='f25', width=0)
        v = json.loads(r[1][1])['values']
        print('states %2d edge %-5s' % (states, edge), [round(x['SI'] / 100, 1) for x in v[:5]], '... min %.1f /cm' % (min(x['SI'] for x in v) / 100))
