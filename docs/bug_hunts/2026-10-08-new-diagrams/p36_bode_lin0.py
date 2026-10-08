"""F1 on the plainest circuit: an RC with a linear AC sweep from 0 Hz, a Bode diagram of v(out), a marker at 1 kHz (Release build: the server dies)."""
import mcp
from common import show
s = mcp.Server('p36')
s.call('new_project', {'name': 'pz'}); s.call('open_project', {'name': 'pz'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '100'}})
s.call('add_component', {'type': 'C', 'name': 'C1', 'x': 400, 'y': 200, 'properties': {'C': '1u'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'C1.1'), ('V1.2', 'ground'), ('C1.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'C1.1', 'name': 'out'})
s.call('add_analysis', {'kind': 'ac', 'from': '0 Hz', 'to': '100 kHz', 'points': 101})
s.call('edit_component', {'name': 'AC1', 'properties': {'Type': 'lin', 'Start': '0 Hz'}})
s.call('save_document', {'as': 'rc0.sch'}); s.call('simulate', {'simulator': 'ngspice'})
print('x from', s.call('get_dataset', {'points': 0}).get('independent variables', [])[:1])
r = s.call('add_diagram', {'type': 'bode', 'traces': ['ac.v(out)']})
try:
    m = s.call('add_marker', {'diagram': r['diagram'], 'at': 1000}); print('marker placed', str(m)[:150])
except Exception:
    print('SERVER DIED, exit', s.p.poll())
s.close()
