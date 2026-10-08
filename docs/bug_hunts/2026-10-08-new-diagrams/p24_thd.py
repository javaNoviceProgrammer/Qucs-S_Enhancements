"""THD of the RC's settled output: the spectrum view's against get_dataset's older thd, and a square wave's THD by hand."""
import json, math, numpy as np, mcp
from common import rc, show
s = mcp.Server('p24')
p, sim = rc(s)
for kw in ({'from': 0.002}, {'from': 0.002, 'harmonics': 9}, {}):
    g = s.call('get_dataset', dict(variables=['tran.v(in)', 'tran.v(out)'], measure=['spectrum', 'thd'], **kw))
    for v in g['variables']:
        m = v['measurements']
        sp, th = m['spectrum'], m['thd']
        print(kw, v['name'], 'spectrum THD', sp.get('THD') or {k: sp.get(k) for k in sp if 'THD' in k or 'thd' in k}, '| thd', {k: th.get(k) for k in th if k in ('value', 'percent', 'THD', 'dB', 'unit', 'fundamental', 'error')})
        print('   harmonics', [(h['harmonic'], h['frequency'], round(h['dBc'], 1)) for h in sp.get('harmonics', [])])
# by hand: an ideal square's THD over harmonics 2..9: odd only, amplitudes 1/n
print('square, harmonics to 9, by hand: %.2f %%' % (100 * math.sqrt(sum((1 / n) ** 2 for n in (3, 5, 7, 9)))))
s.close()
