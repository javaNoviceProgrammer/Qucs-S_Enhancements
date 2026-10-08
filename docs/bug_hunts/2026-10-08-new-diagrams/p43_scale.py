"""The new types on a million-sample transient (a 1 kHz square with noise, as a .npy imported): time per add_diagram and export, Release build."""
import time, numpy as np, mcp
from common import rc, show
s = mcp.Server('p43')
p, sim = rc(s)
n = 1_000_000
t = np.arange(n) * 1e-8                      # 10 ms
rng = np.random.default_rng(1)
v = (np.sign(np.sin(2 * np.pi * 1e3 * t)) + 1) / 2 + rng.normal(0, 0.01, n)
q = np.cos(2 * np.pi * 2.5e3 * t)
np.save(s.ws + '/rc_prj/big.npy', np.column_stack([t, v, q]))
r = s.call('import_data', {'file': s.ws + '/rc_prj/big.npy', 'name': 'big'})
show('import', r, 300)
cols = r.get('columns') if isinstance(r, dict) else None
names = [c.get('trace') or c.get('name') for c in (r.get('variables') or [])] if isinstance(r, dict) else []
print('names', names[:5])
V, Q = 'big:big_1', 'big:big_2'
for typ, tr, kw in (('spectrum', [V], {}), ('spectrogram', [V], {}), ('bathtub', [V], {'bathtub': {'unit_interval': 5e-4}}), ('box_plot', [V], {}),
                    ('constellation', [V, Q], {}), ('tornado', [V, Q], {'tornado': {'mode': 'spread'}}), ('stacked', [V, {'variable': Q, 'pane': 2}], {}), ('eye', [V], {'eye': {'unit_interval': 5e-4}})):
    t0 = time.time(); r = s.call('add_diagram', dict(type=typ, traces=tr, **kw)); t1 = time.time() - t0
    if s.last_error: show(f'{typ} refused', r, 200); continue
    nd = [x.get('no data') for x in r.get('traces', []) if x.get('no data')]
    if nd: print(typ, 'NO DATA', nd[0][:120])
    t0 = time.time(); s.call('export_image', {'diagram': r['diagram'], 'save_as': s.root + f'/{typ}.png'}); t2 = time.time() - t0
    print(f'{typ:14s} add {t1:5.1f} s, export {t2:5.1f} s', flush=True)
t0 = time.time(); s.call('save_document', {}); s.call('close_document', {}); s.call('open_document', {'path': p}); print(f'save, close, reopen: {time.time() - t0:.1f} s')
s.close()
