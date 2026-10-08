"""The box plot's quartiles and the constellation's EVM against numpy on the dataset's own values (the RC run)."""
import json, sys, numpy as np, mcp
from common import rc, show
def read(path):
    blocks, cur, name = {}, None, None
    for l in open(path):
        l = l.strip()
        if l.startswith('<indep ') or l.startswith('<dep '):
            name = l.split(' ')[1]; cur = []
        elif l.startswith('</'):
            blocks[name] = np.array(cur); cur = None
        elif cur is not None and l:
            cur.append(complex(l.replace('+j', '+').replace('-j', '-') + 'j') if 'j' in l else float(l))
    return blocks
s = mcp.Server('p17')
p, sim = rc(s)
d = read(sim['dataset'])
t, vin, vout = d['time'], d['tran.v(in)'], d['tran.v(out)']
r = s.call('add_diagram', {'type': 'box_plot', 'traces': ['tran.v(out)'], 'box_plot': {'whiskers': 'tukey'}})
b = r['box_plot']['boxes'][0]
q1, med, q3 = np.percentile(vout, [25, 50, 75])
iqr = q3 - q1
lo_w = vout[vout >= q1 - 1.5 * iqr].min(); hi_w = vout[vout <= q3 + 1.5 * iqr].max()
print('box  ', {k: b[k] for k in ('q1', 'median', 'q3', 'mean', 'whisker low', 'whisker high', 'values')}, 'outliers', len(b['outliers']) + b.get('more outliers', 0))
print('numpy', dict(q1=q1, median=med, q3=q3, mean=vout.mean(), wl=lo_w, wh=hi_w, n=len(vout)), 'outliers', int(((vout < q1 - 1.5 * iqr) | (vout > q3 + 1.5 * iqr)).sum()))
# EVM: sample I = v(in), Q = v(out) every 0.5 ms from 0.25 ms; QPSK ideal points at the samples' rms
r = s.call('add_diagram', {'type': 'constellation', 'traces': ['tran.v(in)', 'tran.v(out)'], 'constellation': {'symbol_period': 5e-4, 'modulation': 'qpsk'}})
pr = r['constellation']['pairs'][0]
print('constellation', pr)
ts = np.arange(2.5e-4, t[-1] + 1e-12, 5e-4)
I = np.interp(ts, t, vin); Q = np.interp(ts, t, vout)
z = I + 1j * Q
print('by hand: symbols', len(ts), 'centre', [I.mean(), Q.mean()])
rms = np.sqrt(np.mean(np.abs(z) ** 2))
ideal = rms * np.exp(1j * (np.pi / 4 + np.pi / 2 * np.arange(4)))
err = np.min(np.abs(z[:, None] - ideal[None, :]), axis=1)
print('by hand (ideal at the rms, about 0, no centring): EVM rms %', 100 * np.sqrt(np.mean(err ** 2)) / rms, 'peak %', 100 * err.max() / rms)
zc = z - z.mean(); rmsc = np.sqrt(np.mean(np.abs(zc) ** 2)); idealc = rmsc * np.exp(1j * (np.pi / 4 + np.pi / 2 * np.arange(4)))
errc = np.min(np.abs(zc[:, None] - idealc[None, :]), axis=1)
print('by hand (centred first): EVM rms %', 100 * np.sqrt(np.mean(errc ** 2)) / rmsc, 'peak %', 100 * errc.max() / rmsc)
s.close()
