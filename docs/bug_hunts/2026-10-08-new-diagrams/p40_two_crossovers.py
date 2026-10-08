"""Phase margin when the loop gain crosses 0 dB twice: T = 0.5 * H of an RLC of Q 10.5 (R 3, L 1m, C 1u). The margin is the worse crossover's."""
import cmath, math, mcp
from common import show
s = mcp.Server('p40')
s.call('new_project', {'name': 'pm'}); s.call('open_project', {'name': 'pm'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '3'}})
s.call('add_component', {'type': 'L', 'name': 'L1', 'x': 300, 'y': 120, 'properties': {'L': '1m'}})
s.call('add_component', {'type': 'C', 'name': 'C1', 'x': 400, 'y': 200, 'properties': {'C': '1u'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'L1.1'), ('L1.2', 'C1.1'), ('V1.2', 'ground'), ('C1.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'V1.1', 'name': 'in'}); s.call('set_label', {'at': 'C1.1', 'name': 'out'})
s.call('add_analysis', {'kind': 'ac', 'from': '100 Hz', 'to': '100 kHz', 'points': 2001})
s.call('save_document', {'as': 'q.sch'}); s.call('simulate', {'simulator': 'ngspice'})
g = s.call('get_dataset', {'variables': ['0.5*ac.v(out)/ac.v(in)'], 'measure': ['phase_margin', 'gain_margin']})
show('tool', g['variables'][0]['measurements'] if isinstance(g, dict) and 'variables' in g else g, 900)
L, C, R = 1e-3, 1e-6, 3.0
cross = []
prev = None
for i in range(400001):
    f = 100 * 10 ** (i / 400000 * 3)
    w = 2 * math.pi * f
    T = 0.5 / complex(1 - w * w * L * C, w * R * C)
    a = abs(T) - 1
    if prev is not None and (a > 0) != (prev[1] > 0): cross.append((f, 180 + math.degrees(cmath.phase(T))))
    prev = (f, a)
print('by hand: crossovers (f, phase margin there):', [(round(f, 1), round(pm, 2)) for f, pm in cross])
r = s.call('add_diagram', {'type': 'bode', 'traces': ['0.5*ac.v(out)/ac.v(in)']})
s.call('simulate', {'simulator': 'ngspice'})
g = s.call('get_schematic', {})
show('bode margins', [d.get('margins') for d in g['diagrams'] if d['type'] == 'bode'], 900)
s.close()
