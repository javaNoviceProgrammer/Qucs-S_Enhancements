"""The spectrum view's THD of a pure sine (imported, 10.5 periods: not coherent) for each window: it should be near the window's leakage floor."""
import numpy as np, mcp
from common import rc, show
s = mcp.Server('p78')
p, sim = rc(s)
n = 20000; t = np.arange(n) / n * 10.5e-3
np.save(s.ws + '/rc_prj/sine.npy', np.column_stack([t, np.sin(2 * np.pi * 1e3 * t)]))
s.call('import_data', {'file': s.ws + '/rc_prj/sine.npy', 'name': 'sine'})
for w in ('rectangular', 'hann', 'hamming', 'blackman', 'blackman_harris', 'flat_top'):
    r = s.call('add_diagram', {'type': 'spectrum', 'traces': ['sine:sine_1'], 'spectrum': {'window': w}})
    a = (r.get('analyses') or [{}])[0] if isinstance(r, dict) else {}
    print(f"{w:16s} f0 {a.get('fundamental')}  THD {a.get('thd %') or a.get('THD')}  {[(k, a.get(k)) for k in a if 'thd' in k.lower() or 'sfdr' in k.lower()][:3]}  amplitude {a.get('amplitude')}", flush=True)
s.close()
