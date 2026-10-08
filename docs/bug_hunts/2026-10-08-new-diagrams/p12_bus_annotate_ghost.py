"""Buses with odd names; Values at the Marker (moved, deleted, re-simulated); a kept run's ghost (overlay) - on the RC."""
import json, sys, time, mcp
from common import rc, show
s = mcp.Server('p12')
p, sim = rc(s)
def c(label, tool, args, n=420):
    r = s.call(tool, args)
    show(f'[{s.last_time:5.1f}s err={int(s.last_error)}] {label}:', r, n)
    if s.p.poll() is not None: print('SERVER DIED', open(s.root + '/server.err').read()[-3000:]); sys.exit(1)
    return r
for name in ('D[7:0]', 'D[0:7]', 'D[2147483647:0]', 'D[100000:0]', 'D[-5:0]', '[3:0]', 'D[a:b]', 'D[7:0', 'D[7:0][3:0]', 'A,B,C', 'D[3:0],E[1:0]', '', 'gnd[1:0]', 'in[1:0]'):
    c(f'bus {name!r}', 'add_painting', {'type': 'bus', 'name': name, 'points': [[400, 100], [600, 100]]}, 300)
c('check_schematic', 'check_schematic', {}, 900)
# Values at the Marker
d = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(out)']})['diagram']
c('annotate marker', 'add_marker', {'diagram': d, 'at': 0.002, 'annotate': True}, 700)
c('move it', 'edit_marker', {'diagram': d, 'marker': 1, 'at': 0.007}, 500)
g = s.call('get_schematic', {}); show('values shown', {k: v for k, v in g.items() if 'annot' in k or 'value' in k.lower()}, 600)
c('delete the marker', 'delete_marker', {'diagram': d, 'marker': 1}, 300)
g = s.call('get_schematic', {}); show('values after delete', {k: v for k, v in g.items() if 'annot' in k or 'value' in k.lower()}, 600)
c('undo', 'undo', {}, 200)
g = s.call('get_schematic', {}); show('values after undo', {k: v for k, v in g.items() if 'annot' in k or 'value' in k.lower()}, 600)
c('delete the diagram', 'delete', {'diagrams': [d]}, 300)
g = s.call('get_schematic', {}); show('values after the diagram went', {k: v for k, v in g.items() if 'annot' in k or 'value' in k.lower()}, 600)
c('undo', 'undo', {}, 200)
# ghost
c('keep_as before', 'simulate', {'simulator': 'ngspice', 'keep_as': 'before'}, 200)
c('R1 to 10k', 'edit_component', {'name': 'R1', 'properties': {'R': '10k'}}, 200)
c('simulate', 'simulate', {'simulator': 'ngspice'}, 200)
d2 = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(out)']})['diagram']
c('overlay before', 'edit_diagram', {'diagram': d2, 'overlay': 'before'}, 900)
c('overlay nosuch', 'edit_diagram', {'diagram': d2, 'overlay': 'nosuch'}, 300)
c('overlay before again (twice?)', 'edit_diagram', {'diagram': d2, 'overlay': 'before'}, 900)
c('add_trace run before ghost false', 'add_trace', {'diagram': d2, 'variable': 'tran.v(out)', 'run': 'before', 'ghost': False}, 400)
c('overlay on a spectrum', 'add_diagram', {'type': 'spectrum', 'traces': ['tran.v(out)', {'variable': 'tran.v(out)', 'run': 'before'}]}, 900)
c('overlay on a tornado', 'add_diagram', {'type': 'tornado', 'traces': ['tran.v(out)', {'variable': 'tran.v(out)', 'run': 'before'}], 'tornado': {'at': 0.005}}, 900)
c('overlay null', 'edit_diagram', {'diagram': d2, 'overlay': None}, 600)
c('save', 'save_document', {}, 100)
print('alive', s.p.poll() is None, s.root)
s.close()
