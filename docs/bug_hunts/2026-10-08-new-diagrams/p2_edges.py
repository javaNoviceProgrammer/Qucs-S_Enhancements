"""Boundary values of the new diagrams' options on the RC's run: each call timed, its answer cut short."""
import json, sys, mcp
from common import rc, show
s = mcp.Server('p2')
p, sim = rc(s)
V, I = 'tran.v(out)', 'tran.v(in)'
def c(label, tool, args):
    r = s.call(tool, args)
    show(f'[{s.last_time:5.1f}s err={int(s.last_error)}] {label}:', r, 420)
    if s.p.poll() is not None: print('SERVER DIED', open(s.root + '/server.err').read()[-3000:]); sys.exit(1)
    return r
def new(t, tr, **kw):
    r = s.call('add_diagram', dict(type=t, traces=tr, **kw)); return r.get('diagram') if isinstance(r, dict) else None
# stacked panes
d = new('stacked', [V], panes=8)
c('trace into pane 8', 'add_trace', {'diagram': d, 'variable': I, 'pane': 8})
c('panes 8 -> 2 with a trace in pane 8', 'edit_diagram', {'diagram': d, 'panes': 2})
c('get it', 'get_schematic', {'diagram': d} )
for n in (0, 1, 9, -1, [], [{}] * 8 + [{}]):
    c(f'stacked panes={json.dumps(n)[:30]}', 'add_diagram', {'type': 'stacked', 'traces': [V], 'panes': n})
c('trace pane 0', 'add_trace', {'diagram': d, 'variable': V, 'pane': 0})
c('trace pane 3 of 2', 'add_trace', {'diagram': d, 'variable': V, 'pane': 3})
# limits
r = new('rect', [V])
for lim in ([{'upper': [[2e-3, 1], [1e-3, 1]]}], [{'upper': [[1e-3, 1]]}], [{'upper': []}], [{'lower': 'abc'}], [{'upper': 0.5, 'lower': 0.1}],
            [{'upper': [[0, 0.5], [0.005, 0.5], [0.005, 0.4], [0.01, 0.4]]}], [{'upper': 1e308}, {'lower': -1e308}], [{'upper': 0.5, 'pane': 3}],
            [{'upper': [[0, 1]] * 3}]):
    c(f'limits {json.dumps(lim)[:70]}', 'edit_diagram', {'diagram': r, 'limits': lim})
# delta markers
m1 = c('marker 1', 'add_marker', {'diagram': r, 'at': 0.002})
c('marker relative to itself (1)', 'add_marker', {'diagram': r, 'at': 0.004, 'relative_to': 2})
c('marker 3 rel 99', 'add_marker', {'diagram': r, 'at': 0.004, 'relative_to': 99})
c('edit marker 1 rel 2 (a loop?)', 'edit_marker', {'diagram': r, 'marker': 1, 'relative_to': 2})
c('edit marker 1 rel 1', 'edit_marker', {'diagram': r, 'marker': 1, 'relative_to': 1})
c('get markers', 'get_schematic', {'diagram': r})
c('delete marker 1 (a reference)', 'delete_marker', {'diagram': r, 'marker': 1})
c('get markers after', 'get_schematic', {'diagram': r})
# spectrogram, spectrum, bathtub, constellation: sizes
c('spectrogram segment 1e-9', 'add_diagram', {'type': 'spectrogram', 'traces': [V], 'spectrogram': {'segment': 1e-9}})
c('spectrogram overlap 0.999999', 'add_diagram', {'type': 'spectrogram', 'traces': [V], 'spectrogram': {'overlap': 0.999999}})
c('spectrogram segment 1 (longer than the run)', 'add_diagram', {'type': 'spectrogram', 'traces': [V], 'spectrogram': {'segment': 1}})
c('spectrogram range 0', 'add_diagram', {'type': 'spectrogram', 'traces': [V], 'spectrogram': {'range': 0}})
c('spectrum fundamental 1e-9', 'add_diagram', {'type': 'spectrum', 'traces': [V], 'spectrum': {'fundamental': 1e-9}})
c('spectrum from past the end', 'add_diagram', {'type': 'spectrum', 'traces': [V], 'spectrum': {'from': 1}})
c('spectrum harmonics 50 fundamental 4e5', 'add_diagram', {'type': 'spectrum', 'traces': [V], 'spectrum': {'harmonics': 50, 'fundamental': 4e5}})
c('bathtub ui 1e-15', 'add_diagram', {'type': 'bathtub', 'traces': [I], 'bathtub': {'unit_interval': 1e-15}})
c('bathtub ui 1e-3', 'add_diagram', {'type': 'bathtub', 'traces': [I], 'bathtub': {'unit_interval': 1e-3}})
c('bathtub ui -1', 'add_diagram', {'type': 'bathtub', 'traces': [I], 'bathtub': {'unit_interval': -1}})
c('bathtub ui abc', 'add_diagram', {'type': 'bathtub', 'traces': [I], 'bathtub': {'unit_interval': 'abc'}})
c('bathtub floor 0', 'add_diagram', {'type': 'bathtub', 'traces': [I], 'bathtub': {'unit_interval': 1e-3, 'floor': 0}})
c('constellation period 1e-15', 'add_diagram', {'type': 'constellation', 'traces': [I, V], 'constellation': {'symbol_period': 1e-15}})
c('constellation 3 traces', 'add_diagram', {'type': 'constellation', 'traces': [I, V, I], 'constellation': {'modulation': '64qam'}})
c('box_plot at abc', 'add_diagram', {'type': 'box_plot', 'traces': [V], 'box_plot': {'at': 'abc'}})
c('tornado at 0.005', 'add_diagram', {'type': 'tornado', 'traces': [V, I], 'tornado': {'at': 0.005}})
c('tornado spread', 'add_diagram', {'type': 'tornado', 'traces': [V, I], 'tornado': {'mode': 'spread'}})
c('tornado at 99', 'add_diagram', {'type': 'tornado', 'traces': [V, I], 'tornado': {'at': 99}})
c('get_dataset spectrum', 'get_dataset', {'variables': [V], 'measure': ['spectrum', 'thd']})
c('get_dataset bathtub ui 1e-3', 'get_dataset', {'variables': [I], 'measure': ['bathtub'], 'bit_period': 1e-3})
c('get_dataset evm', 'get_dataset', {'variables': [I], 'measure': ['evm'], 'evm': {'q': V, 'symbol_period': 1e-15}})
c('save', 'save_document', {})
c('reopen', 'open_document', {'path': p})
print('server alive:', s.p.poll() is None)
s.close()
