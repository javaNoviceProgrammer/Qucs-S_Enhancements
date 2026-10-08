"""A Bode diagram's marker placed on a log AC sweep (no crash), then the sweep made linear from 0 Hz and simulated again; then the file reopened (Release build)."""
import mcp
from common import show
s = mcp.Server('p37')
s.call('new_project', {'name': 'pz'}); s.call('open_project', {'name': 'pz'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '100'}})
s.call('add_component', {'type': 'C', 'name': 'C1', 'x': 400, 'y': 200, 'properties': {'C': '1u'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'C1.1'), ('V1.2', 'ground'), ('C1.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'C1.1', 'name': 'out'})
s.call('add_analysis', {'kind': 'ac', 'from': '10 Hz', 'to': '100 kHz', 'points': 101})
s.call('save_document', {'as': 'rc.sch'}); s.call('simulate', {'simulator': 'ngspice'})
r = s.call('add_diagram', {'type': 'bode', 'traces': ['ac.v(out)']})
show('marker on the log sweep', s.call('add_marker', {'diagram': r['diagram'], 'at': 1000}), 120)
s.call('edit_component', {'name': 'AC1', 'properties': {'Type': 'lin', 'Start': '0 Hz'}})
s.call('save_document', {})
try:
    sim = s.call('simulate', {'simulator': 'ngspice'}); print('simulated again (lin from 0 Hz):', str(sim)[:120])
    g = s.call('get_schematic', {}); print('get_schematic:', str(g)[:80])
except Exception:
    print('SERVER DIED after the run, exit', s.p.poll())
s.close()
path = s.ws + '/pz_prj/rc.sch'
s2 = mcp.Server('p37b')
try:
    r = s2.call('open_document', {'path': path}); print('reopened in a new server:', str(r)[:120])
    g = s2.call('get_schematic', {}); print('get_schematic:', str(g)[:80])
except Exception:
    print('SERVER DIED opening the file, exit', s2.p.poll())
s2.close()
