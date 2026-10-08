"""The Nichols chart's export on an ordinary loop gain: the RLC's 10*v(out)/v(in), log AC 100 Hz to 1 MHz (gain +20 to about -100 dB)."""
import os, mcp
from common import show
s = mcp.Server('p75')
s.call('new_project', {'name': 'pz'}); s.call('open_project', {'name': 'pz'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '100'}})
s.call('add_component', {'type': 'L', 'name': 'L1', 'x': 300, 'y': 120, 'properties': {'L': '1m'}})
s.call('add_component', {'type': 'C', 'name': 'C1', 'x': 400, 'y': 200, 'properties': {'C': '1u'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'L1.1'), ('L1.2', 'C1.1'), ('V1.2', 'ground'), ('C1.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'V1.1', 'name': 'in'}); s.call('set_label', {'at': 'C1.1', 'name': 'out'})
s.call('add_analysis', {'kind': 'ac', 'from': '100 Hz', 'to': '1 MHz', 'points': 201})
s.call('save_document', {'as': 'rlc.sch'}); s.call('simulate', {'simulator': 'ngspice'})
r = s.call('add_diagram', {'type': 'nichols', 'traces': ['ac.v(out)']})
for f in ('svg', 'pdf'):
    s.call('export_image', {'diagram': r['diagram'], 'save_as': s.root + f'/n.{f}'}); print(f, os.path.getsize(s.root + f'/n.{f}'))
s.close()
