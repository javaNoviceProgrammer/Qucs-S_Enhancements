"""add_marker on every new type (the RC's data; the FFT's for the frequency types), then moved: refused, placed, or a crash? ASan build."""
import mcp
from common import rc, show
T, F = 'tran.v(out)', 'ac.v(out)'
specs = [('stacked', [T, {'variable': 'tran.v(in)', 'pane': 2}], {}), ('bode', [F], {}), ('nichols', [F], {}), ('pole_zero', [F], {}), ('bathtub', ['tran.v(in)'], {'bathtub': {'unit_interval': 5e-4}}),
         ('contour', [T], {}), ('spectrogram', [T], {}), ('tornado', [T, 'tran.v(in)'], {}), ('box_plot', [T], {}), ('constellation', ['tran.v(in)', T], {}),
         ('polar', [F], {'nyquist': {'marks': True, 'mirror': True}}), ('smith', [F], {})]
s = mcp.Server('p68')
p, sim = rc(s)
for t, tr, kw in specs:
    r = s.call('add_diagram', dict(type=t, traces=tr, **kw))
    try:
        m = s.call('add_marker', {'diagram': r['diagram'], 'at': 0.003 if t not in ('bode', 'nichols', 'polar', 'smith', 'pole_zero') else 1000, 'trace': 1})
        a = 'refused: ' + str(m)[:90] if s.last_error else 'placed'
        if not s.last_error:
            e = s.call('edit_marker', {'diagram': r['diagram'], 'marker': 1, 'at': 0.004 if t not in ('bode', 'nichols', 'polar', 'smith', 'pole_zero') else 2000})
            a += ', moved' if not s.last_error else ', move refused: ' + str(e)[:60]
        print(f'{t:14s} {a}', flush=True)
    except Exception:
        print(f'{t:14s} SERVER DIED', [l for l in open(s.root + '/server.err', errors='replace').read().splitlines() if 'SEGV' in l or 'runtime error' in l][:2], flush=True)
        s = mcp.Server('p68'); p, sim = rc(s)
err = open(s.root + '/server.err', errors='replace').read()
print('sanitizer (last server):', [l for l in err.splitlines() if 'runtime error' in l][:3])
s.close()
