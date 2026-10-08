"""Simulation > Probe through the 'probe' tool: odd targets, odd diagrams; then a run with what it asked to be saved."""
import json, sys, mcp
from common import rc, show
s = mcp.Server('p8')
p, sim = rc(s)
def c(label, tool, args, n=330):
    r = s.call(tool, args)
    show(f'[{s.last_time:4.1f}s err={int(s.last_error)}] {label}:', r, n)
    if s.p.poll() is not None: print('SERVER DIED', open(s.root + '/server.err').read()[-3000:]); sys.exit(1)
    return r
for what in ('gnd', 'ground', '0', 'TR1', 'NutmegEq1', 'FFT1', [5000, 5000], [200, 150], [-1e9, 1e9], 'R99', 'R1.3', 'R1.0', 'R1.-1', 'V1', 'C1.2', 'in', ''):
    c(f'probe {json.dumps(what)}', 'probe', {'what': what, 'diagram': 1})
c('probe a list', 'probe', {'what': ['R1', 'C1', 'out', 'V1.1'], 'diagram': 1})
c('probe diagram 99', 'probe', {'what': 'out', 'diagram': 99})
c('probe diagram 0', 'probe', {'what': 'out', 'diagram': 0})
for t in ('tab', 'truth', 'timing', 'pole_zero', 'contour', 'tornado', 'constellation', 'smith', 'bathtub', '3d', 'histogram', 'eye', 'locus'):
    d = s.call('add_diagram', {'type': t})
    n = d.get('diagram') if isinstance(d, dict) else None
    c(f'probe out into {t} (#{n})', 'probe', {'what': 'out', 'diagram': n}, 260)
    c(f'probe R1 (power) into {t}', 'probe', {'what': 'R1', 'diagram': n}, 200)
c('netlist after probing currents and powers', 'get_netlist', {}, 2500)
c('simulate', 'simulate', {'simulator': 'ngspice'}, 300)
g = s.call('get_schematic', {})
show('diagram 1 traces', [(t['variable'], t.get('points'), t.get('no data', '')[:80]) for t in g['diagrams'][0]['traces']], 1500)
print('alive', s.p.poll() is None, s.root)
s.close()
