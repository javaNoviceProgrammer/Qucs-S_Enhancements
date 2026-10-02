import sys, time, math, random; sys.path.insert(0, sys.argv[1])
from probe import Probe, sch
import json
root = sys.argv[1] + '/p4'
p = Probe(root)
def dataset(name, t, v):
    lines = ['<Qucs Dataset 26.1.5>', '<indep time %d>' % len(t)] + ['  %.12e' % x for x in t] + ['</indep>', '<dep tran.v(out) time>'] + \
            ['  %s' % ('nan' if math.isnan(x) else '%.12e' % x) for x in v] + ['</dep>']
    open(root + '/ws/p/' + name, 'w').write('\n'.join(lines) + '\n')
random.seed(3); t=[]; v=[]
for b in range(300):
    bit = random.randint(0, 1)
    for k in range(20):
        t.append((b*20+k)*5e-12); v.append(float(bit))
# equal consecutive times at each bit boundary (a breakpoint written twice)
t2=[]; v2=[]
for i,(a,b) in enumerate(zip(t,v)):
    t2.append(a); v2.append(b)
    if i % 20 == 19: t2.append(a); v2.append(b)
dataset('e.dat.ngspice', t2, v2)
print(p.open('e.sch', sch('  <GND * 1 0 0 0 0 0 0>\n'))[0])
e, out = p.call('get_dataset', {'variables': ['tran.v(out)'], 'measure': ['eye']})
print('dup times:', e, out[:300])
e, out = p.call('add_diagram', {'type': 'eye', 'x': 100, 'y': 400, 'traces': ['tran.v(out)']})
print('add:', e, out[:200])
for eye in [{'unit_interval': -1}, {'unit_interval': 0}, {'unit_interval': '1e400'}, {'span': 0}, {'span': 9}, {'span': 8},
            {'mask': {'width': 2, 'height': 0.1}}, {'mask': {'width': 0.5, 'height': -1}}, {'mask': {'width': 0, 'height': 0.1}},
            {'mask': {'width': 1, 'height': 1e30}}, {'unit_interval': '1p'}, {'unit_interval': 1e-6}, {'drawn as': 'x'}]:
    e, out = p.call('edit_diagram', {'diagram': 1, 'eye': eye})
    e2, shot = p.call('screenshot', {})
    print('%-40s err=%s %s | shot %s' % (json.dumps(eye)[:40], e, out[:150].replace('\n', ' '), 'ok' if not e2 else shot[:80]))
e, out = p.call('get_schematic', {'format': 'json'})
d = json.loads(out).get('diagrams', [{}])[0]
print('diagram:', json.dumps(d)[:600])
p.close()
