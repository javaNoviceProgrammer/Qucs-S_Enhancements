"""Every new diagram type on the RC's runs, with its defaults; get_schematic and export_image of each."""
import json, mcp
from common import rc, show
s = mcp.Server('p1')
p, sim = rc(s, ac=True)
show('sim', sim, 600)
show('vars', s.call('get_dataset', {'points': 0}), 800)
for t, tr in [('stacked', ['tr1.tran.v(in)', {'variable': 'tr1.tran.v(out)', 'pane': 2}]), ('pole_zero', ['ac1.ac.v(out)']), ('bode', ['ac1.ac.v(out)']),
              ('nichols', ['ac1.ac.v(out)']), ('spectrum', ['tr1.tran.v(out)']), ('bathtub', ['tr1.tran.v(out)']), ('contour', ['tr1.tran.v(out)']),
              ('spectrogram', ['tr1.tran.v(out)']), ('tornado', ['tr1.tran.v(out)', 'tr1.tran.v(in)']), ('box_plot', ['tr1.tran.v(out)']),
              ('constellation', ['tr1.tran.v(in)', 'tr1.tran.v(out)']), ('polar', ['ac1.ac.v(out)'])]:
    r = s.call('add_diagram', {'type': t, 'traces': tr})
    show(f'{t}: err={s.last_error}', r, 700)
g = s.call('get_schematic', {})
for d in g.get('diagrams', []):
    show(f"  #{d.get('number')} {d.get('type')}", {k: v for k, v in d.items() if k not in ('traces',)}, 500)
for i in range(1, 13):
    r = s.call('export_image', {'diagram': i, 'save_as': s.root + f'/d{i}.png'})
    if s.last_error: show(f'export {i}', r, 300)
print('server alive:', s.p.poll() is None)
s.close()
