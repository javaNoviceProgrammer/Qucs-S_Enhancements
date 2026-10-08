"""Delta markers (a pair, a loop, the reference deleted, undo), panes taken away under their traces, and a save and reopen of it all."""
import json, sys, mcp
from common import rc, show
s = mcp.Server('p4')
p, sim = rc(s)
V, I = 'tran.v(out)', 'tran.v(in)'
def c(label, tool, args, n=420):
    r = s.call(tool, args)
    show(f'[{s.last_time:5.1f}s err={int(s.last_error)}] {label}:', r, n)
    if s.p.poll() is not None: print('SERVER DIED', open(s.root + '/server.err').read()[-3000:]); sys.exit(1)
    return r
def diagram(n):
    return next(d for d in s.call('get_schematic', {})['diagrams'] if d['diagram'] == n)
r = s.call('add_diagram', {'type': 'rect', 'traces': [V, I]})['diagram']
c('m1', 'add_marker', {'diagram': r, 'at': 0.002, 'trace': 1})
c('m2 rel 1', 'add_marker', {'diagram': r, 'at': 0.004, 'relative_to': 1, 'trace': 1})
c('m3 rel 2', 'add_marker', {'diagram': r, 'at': 0.006, 'relative_to': 2, 'trace': 2})
c('m1 rel 3: a loop 1->3->2->1', 'edit_marker', {'diagram': r, 'marker': 1, 'relative_to': 3})
show('markers', diagram(r).get('markers'), 1500)
c('delete m2 (the reference of m3)', 'delete_marker', {'diagram': r, 'marker': 2})
show('markers after', diagram(r).get('markers'), 1500)
c('undo', 'undo', {})
show('markers after undo', diagram(r).get('markers'), 1500)
c('delete trace 1 (m1, m2 on it)', 'delete', {'diagrams': [r], 'traces': [1]} )
show('markers after the trace went', diagram(r).get('markers'), 1500)
# panes
d = s.call('add_diagram', {'type': 'stacked', 'traces': [V], 'panes': 4})['diagram']
c('trace pane 4', 'add_trace', {'diagram': d, 'variable': I, 'pane': 4})
c('trace pane 5 of 4', 'add_trace', {'diagram': d, 'variable': 'tran.v(out)/2', 'pane': 5})
c('trace pane 0', 'add_trace', {'diagram': d, 'variable': 'tran.v(out)*2', 'pane': 0})
c('marker on pane 4 trace', 'add_marker', {'diagram': d, 'at': 0.003, 'trace': 2})
c('limit on pane 4', 'edit_diagram', {'diagram': d, 'limits': [{'upper': 0.9, 'pane': 4}]})
c('panes 4 -> 2', 'edit_diagram', {'diagram': d, 'panes': 2})
show('stacked after', {k: diagram(d).get(k) for k in ('panes', 'traces', 'markers', 'limits', 'verdict')}, 2500)
c('save', 'save_document', {})
c('close', 'close_document', {})
c('reopen', 'open_document', {'path': p})
show('rect after reopen', {k: diagram(r).get(k) for k in ('markers',)}, 1500)
show('stacked after reopen', {k: diagram(d).get(k) for k in ('panes', 'traces', 'markers', 'limits', 'verdict')}, 2500)
c('export stacked', 'export_image', {'diagram': d, 'save_as': s.root + '/stacked.png'}, 200)
print('server alive:', s.p.poll() is None, s.root)
s.close()
