"""Diagram Properties for the new types through the context menu: what the dialog shows, and OK pressed unchanged - is the diagram the same after?"""
import json, sys, mcp
from common import rc, show
s = mcp.Server('p23')
p, sim = rc(s)
V, I = 'tran.v(out)', 'tran.v(in)'
specs = [('stacked', [V, {'variable': I, 'pane': 3}], {'panes': 3, 'limits': [{'upper': 0.9, 'pane': 3, 'label': 'max'}]}),
         ('spectrum', [V], {'spectrum': {'window': 'flat_top', 'harmonics': 5, 'drawn': 'stems', 'fundamental': 1000, 'from': 0.002}}),
         ('spectrogram', [V], {'spectrogram': {'segment': 0.001, 'overlap': 0.25, 'range': 60, 'window': 'blackman'}}),
         ('bathtub', [I], {'bathtub': {'unit_interval': 5e-4, 'ber': 1e-9, 'floor': 1e-14}}),
         ('tornado', [V, I], {'tornado': {'mode': 'spread', 'at': 0.004, 'bars': 7}}),
         ('box_plot', [V], {'box_plot': {'whiskers': 'range', 'at': 0.003}}),
         ('constellation', [I, V], {'constellation': {'modulation': '16qam', 'symbol_period': 5e-4, 'offset': 1e-4}}),
         ('contour', [V], {'contour': {'map': 'grey', 'levels': 5, 'filled': False, 'labels': False, 'range': {'from': 0, 'to': 1}, 'pass': {'min': 0.2}}}),
         ('bode', [V], {'bode': {'margins': False}}), ('nichols', [V], {'nichols': {'grid': False}}), ('pole_zero', [V], {'pole_zero': {'guides': False}}),
         ('polar', [V], {'nyquist': {'marks': True, 'mirror': True}})]
def strip(d):
    return {k: v for k, v in d.items() if k not in ('note', 'analyses', 'roots', 'margins', 'verdict')}
for t, tr, kw in specs:
    r = s.call('add_diagram', dict(type=t, traces=tr, **kw))
    if s.last_error: show(f'{t} refused', r, 300); continue
    n = r['diagram']
    before = strip(next(d for d in s.call('get_schematic', {})['diagrams'] if d['diagram'] == n))
    m = s.call('context_menu', {'on': {'diagram': n}, 'choose': 'Edit Properties'})
    dlg = s.call('get_dialog', {})
    title = dlg.get('title') if isinstance(dlg, dict) else str(dlg)[:100]
    ok = s.call('set_dialog', {'press': 'OK'})
    if s.last_error: show(f'{t}: OK', ok, 300); s.call('set_dialog', {'press': 'Cancel'})
    after = strip(next(d for d in s.call('get_schematic', {})['diagrams'] if d['diagram'] == n))
    diff = {k: (before.get(k), after.get(k)) for k in set(before) | set(after) if before.get(k) != after.get(k)}
    show(f'{t}: dialog {title!r}; changed by OK unchanged:', diff or 'nothing', 1500)
    if s.p.poll() is not None: print('SERVER DIED'); sys.exit(1)
print('alive', s.p.poll() is None, s.root)
s.close()
