import sys, time, math, random; sys.path.insert(0, sys.argv[1])
from probe import Probe, sch
import json
root = sys.argv[1] + '/p3'
p = Probe(root)
def dataset(name, t, v):
    lines = ['<Qucs Dataset 26.1.5>', '<indep time %d>' % len(t)] + ['  %.12e' % x for x in t] + ['</indep>', '<dep tran.v(out) time>'] + \
            ['  %s' % ('nan' if isinstance(x, float) and math.isnan(x) else '%.12e' % x) for x in v] + ['</dep>']
    open(root + '/ws/p/' + name, 'w').write('\n'.join(lines) + '\n')
def nrz(n, ui=1e-10, pts=20, seed=1):
    random.seed(seed); t=[]; v=[]
    for b in range(n):
        bit = random.randint(0, 1)
        for k in range(pts):
            t.append((b*pts+k)*ui/pts); v.append(float(bit))
    return t, v
cases = {}
t, v = nrz(200); cases['nrz'] = (t, v)
cases['flat'] = ([i*1e-11 for i in range(2000)], [0.5]*2000)
cases['oneedge'] = ([i*1e-11 for i in range(2000)], [0.0]*1000 + [1.0]*1000)
cases['twopts'] = ([0.0, 1e-9], [0.0, 1.0])
cases['onept'] = ([0.0], [1.0])
t, v = nrz(200); v = [float('nan') if i % 7 == 0 else x for i, x in enumerate(v)]; cases['nans'] = (t, v)
t, v = nrz(200); t = t[::-1]; cases['backwards'] = (t, v)
t, v = nrz(200); t[500] = t[400]; cases['repeatT'] = (t, v)
t, v = nrz(100000, pts=20); cases['big2M'] = (t, v)
p.open('e.sch', sch('  <GND * 1 0 0 0 0 0 0>\n'))
argsets = [
  {}, {'bit_period': 1e-10}, {'bit_period': 0}, {'bit_period': -1e-10}, {'bit_period': 'abc'}, {'bit_period': 1e-3},
  {'bit_period': 1e-15}, {'levels': 4}, {'levels': 3}, {'levels': 0}, {'levels': 1000}, {'offset': 1e9}, {'level': 'x'},
]
for name, (t, v) in cases.items():
    dataset(name + '.dat.ngspice', t, v)
    sets = argsets if name in ('nrz',) else [{}, {'bit_period': 1e-10}, {'levels': 4}]
    for a in sets:
        args = {'path': root + '/ws/p/' + name + '.dat.ngspice', 'variables': ['tran.v(out)'], 'measure': ['eye']}
        args.update(a)
        t0 = time.time()
        try:
            e, out = p.call('get_dataset', args)
        except Exception as ex:
            print(name, a, 'SERVER DIED', ex); raise
        dt = time.time() - t0
        try:
            j = json.loads(out); m = j['variables'][0].get('measurements', {}).get('eye', j['variables'][0].get('measurements'))
            s = json.dumps(m)[:230]
        except Exception:
            s = out[:230].replace('\n', ' ')
        print('%-9s %-22s %5.2fs err=%s %s' % (name, json.dumps(a)[:22], dt, e, s))
p.close()
