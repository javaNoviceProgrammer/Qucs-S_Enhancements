"""A seed for the schematic fuzzer: the RC with every new diagram type, markers (a delta pair), limits, a ghost; saved with its dataset."""
import os, shutil, sys, mcp
from common import rc, show
s = mcp.Server('p6')
p, sim = rc(s)
V, I = 'tran.v(out)', 'tran.v(in)'
d = s.call('add_diagram', {'type': 'stacked', 'traces': [V, {'variable': I, 'pane': 3}], 'panes': 3,
                           'limits': [{'upper': [[0, 0.7], [0.005, 0.7], [0.005, 0.5], [0.01, 0.5]], 'label': 'mask', 'pane': 1}, {'lower': 0.1, 'pane': 3}]})['diagram']
s.call('add_marker', {'diagram': d, 'at': 0.002, 'trace': 1}); s.call('add_marker', {'diagram': d, 'at': 0.004, 'trace': 1, 'relative_to': 1})
for t, tr, kw in [('bode', [V], {}), ('nichols', [V], {}), ('pole_zero', [V], {}), ('spectrum', [V], {'spectrum': {'drawn': 'stems', 'window': 'flat_top'}}),
                  ('bathtub', [I], {'bathtub': {'unit_interval': 5e-4}}), ('contour', [V], {}), ('spectrogram', [V], {'spectrogram': {'segment': 1e-3}}),
                  ('tornado', [V, I], {'tornado': {'mode': 'spread'}}), ('box_plot', [V, I], {}), ('constellation', [I, V], {'constellation': {'modulation': 'qpsk', 'symbol_period': 5e-4}}),
                  ('polar', [V], {'nyquist': {'marks': True, 'mirror': True}})]:
    r = s.call('add_diagram', dict(type=t, traces=tr, **kw))
    if s.last_error: show(t, r, 300)
s.call('add_painting', {'type': 'bus', 'name': 'D[3:0]', 'points': [[400, 100], [600, 100]]})
show('save', s.call('save_document', {}), 200)
seeds = sys.argv[1]; os.makedirs(seeds, exist_ok=True)
for f in os.listdir(os.path.dirname(p)):
    if f.endswith(('.sch', '.ngspice', '.dat', '.dpl')): shutil.copy(os.path.join(os.path.dirname(p), f), seeds)
print(sorted(os.listdir(seeds)))
s.close()
