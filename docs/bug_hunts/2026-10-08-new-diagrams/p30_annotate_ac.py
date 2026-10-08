"""Values at the Marker on an AC sweep (the RLC of p15): complex values on the schematic, checked against v(out)/v(in) by hand."""
import cmath, math, mcp
from common import show
s = mcp.Server('p30')
s.call('new_project', {'name': 'pz'}); s.call('open_project', {'name': 'pz'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '100'}})
s.call('add_component', {'type': 'L', 'name': 'L1', 'x': 300, 'y': 120, 'properties': {'L': '1m'}})
s.call('add_component', {'type': 'C', 'name': 'C1', 'x': 400, 'y': 200, 'properties': {'C': '1u'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'L1.1'), ('L1.2', 'C1.1'), ('V1.2', 'ground'), ('C1.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'V1.1', 'name': 'in'}); s.call('set_label', {'at': 'C1.1', 'name': 'out'}); s.call('set_label', {'at': 'R1.2', 'name': 'mid'})
s.call('add_analysis', {'kind': 'ac', 'from': '100 Hz', 'to': '1 MHz', 'points': 201})
s.call('save_document', {'as': 'rlc.sch'}); s.call('simulate', {'simulator': 'ngspice'})
d = s.call('add_diagram', {'type': 'rect', 'traces': ['ac.v(out)'], 'x_axis': {'log': True}})['diagram']
m = s.call('add_marker', {'diagram': d, 'at': 5000, 'annotate': True})
show('marker', m, 900)
f = m['at']['frequency'] if isinstance(m.get('at'), dict) and 'frequency' in m['at'] else 5000
w = 2 * math.pi * f
H = 1 / complex(1 - w * w * 1e-3 * 1e-6, w * 100 * 1e-6)
Zc = 1 / complex(0, w * 1e-6); Zl = complex(0, w * 1e-3)
mid = (Zl + Zc) / (100 + Zl + Zc)
print(f'by hand at {f:.1f} Hz: out {abs(H):.5f} at {math.degrees(cmath.phase(H)):.2f} deg; mid {abs(mid):.5f} at {math.degrees(cmath.phase(mid)):.2f} deg')
s.close()
