"""Log axes over data from 0 Hz (the FFT's) and below zero, on every new type that draws a log axis; each exported, on the ASan build."""
import mcp
from common import rc, show
s = mcp.Server('p27')
p, sim = rc(s)
F = 'ac.v(out)'   # the FFT's: 0 Hz to 1 MHz
specs = [('bode', [F], {}), ('stacked', [F], {'x_axis': {'log': True}, 'panes': [{'y_axis': {'log': True}}, {}]}), ('nichols', [F], {}),
         ('rect', [F], {'x_axis': {'log': True}, 'y_axis': {'log': True}, 'limits': [{'upper': [[0, 1], [1e3, 1], [1e6, 0.001]]}]}),
         ('bathtub', ['tran.v(in)'], {'bathtub': {'unit_interval': 5e-4}, 'y_axis': {'log': True, 'auto': False, 'from': 0, 'to': 1}}),
         ('spectrum', ['tran.v(out)'], {'x_axis': {'log': True}}), ('tornado', ['tran.v(out)', 'tran.v(in)'], {'x_axis': {'log': True}}),
         ('box_plot', ['tran.v(out)'], {'y_axis': {'log': True}}), ('constellation', ['tran.v(in)', 'tran.v(out)'], {'x_axis': {'log': True}, 'y_axis': {'log': True}})]
for t, tr, kw in specs:
    r = s.call('add_diagram', dict(type=t, traces=tr, **kw))
    if s.last_error: show(f'{t} refused', r, 200); continue
    e = s.call('export_image', {'diagram': r['diagram'], 'save_as': s.root + f'/{t}.png'})
    show(f'{t} exported err={int(s.last_error)}', e if s.last_error else 'ok', 200)
    if s.p.poll() is not None: print('SERVER DIED'); break
err = open(s.root + '/server.err', errors='replace').read()
print('sanitizer lines:', [l for l in err.splitlines() if 'runtime error' in l or 'Sanitizer' in l][:8])
print('alive', s.p.poll() is None, s.root)
s.close()
