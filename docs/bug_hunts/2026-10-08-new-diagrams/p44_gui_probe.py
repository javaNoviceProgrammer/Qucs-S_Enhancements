"""Simulation > Probe in the window (trigger_action, then clicks with send_input): on a wire, a pin, a part, empty canvas, a diagram, a simulation block; Insert > Bus with clicks; Values at the Marker with no marker - ASan build."""
import sys, mcp
from common import rc, show
s = mcp.Server('p44')
p, sim = rc(s)
g = s.call('get_schematic', {})
r1 = next(c for c in g['components'] if c['name'] == 'R1'); c1 = next(c for c in g['components'] if c['name'] == 'C1')
d1 = g['diagrams'][0]
w = g['wires'][2]; mid = [(w['x1'] + w['x2']) // 2, (w['y1'] + w['y2']) // 2]
def c(label, tool, args, n=200):
    r = s.call(tool, args)
    show(f'[err={int(s.last_error)}] {label}:', r, n)
    if s.p.poll() is not None: print('SERVER DIED'); sys.exit(1)
c('Probe on', 'trigger_action', {'action': 'Simulation > Probe'})
for label, at in (('a wire', mid), ('R1 pin 1', [r1['pins'][0]['x'], r1['pins'][0]['y']]), ('R1 body', [r1['x'], r1['y']]), ('C1 body', [c1['x'], c1['y']]),
                  ('empty canvas', [5000, 5000]), ('the diagram', [d1['x'] + 50, d1['y'] - 50]), ('TR1 block', [-30, 320])):
    c(f'click {label} {at}', 'send_input', {'click': at})
c('Escape', 'send_input', {'keys': 'Escape'})
c('Probe again, then right click', 'trigger_action', {'action': 'Simulation > Probe'})
c('right click', 'send_input', {'click': mid, 'button': 'right'})
c('Escape', 'send_input', {'keys': 'Escape, Escape'})
c('Insert > Bus', 'trigger_action', {'action': 'Insert > Bus'})
c('bus click 1', 'send_input', {'click': [600, 600]})
c('bus click 2 (same point)', 'send_input', {'click': [600, 600]})
c('bus double', 'send_input', {'click': [800, 600], 'double': True})
c('Escape', 'send_input', {'keys': 'Escape'})
c('Values at the Marker (no marker)', 'trigger_action', {'action': 'Simulation > Values at the Marker'})
c('Colour wires on', 'trigger_action', {'action': 'View > Colour Wires by Net'})
c('screenshot', 'screenshot', {'save_as': s.root + '/win.png'} , 120)
g = s.call('get_schematic', {})
show('traces of diagram 1 now', [t['variable'] for t in g['diagrams'][0]['traces']], 600)
show('paintings', [x for x in g.get('paintings', [])], 600)
err = open(s.root + '/server.err', errors='replace').read()
print('sanitizer:', [l for l in err.splitlines() if 'runtime error' in l or 'Sanitizer' in l][:4], 'alive', s.p.poll() is None, s.root)
s.close()
