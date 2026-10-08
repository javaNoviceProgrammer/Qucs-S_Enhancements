"""A marker on a Bode diagram of a real AC run (the RLC's v(out)/v(in)), Release build: the crash of p34 on the plainest case; and in the GUI's file (a saved marker)."""
import os, sys, subprocess, mcp
from common import show
s = mcp.Server('p35')
s.call('new_project', {'name': 'pz'}); s.call('open_project', {'name': 'pz'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '100'}})
s.call('add_component', {'type': 'L', 'name': 'L1', 'x': 300, 'y': 120, 'properties': {'L': '1m'}})
s.call('add_component', {'type': 'C', 'name': 'C1', 'x': 400, 'y': 200, 'properties': {'C': '1u'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'L1.1'), ('L1.2', 'C1.1'), ('V1.2', 'ground'), ('C1.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'C1.1', 'name': 'out'})
s.call('add_analysis', {'kind': 'ac', 'from': '100 Hz', 'to': '1 MHz', 'points': 201})
s.call('save_document', {'as': 'rlc.sch'}); s.call('simulate', {'simulator': 'ngspice'})
r = s.call('add_diagram', {'type': 'bode', 'traces': ['ac.v(out)']})
show('bode', {k: r.get(k) for k in ('diagram', 'traces', 'margins')}, 400)
s.call('save_document', {})
path = s.ws + '/pz_prj/rlc.sch'
try:
    m = s.call('add_marker', {'diagram': r['diagram'], 'at': 5000})
    print('marker placed:', str(m)[:300])
except Exception:
    print('SERVER DIED, exit', s.p.poll())
s.close()
