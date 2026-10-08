"""What set_label makes of rail names (+5V, -12V, 3V3, V+, VDD_IO), and the wire kind each then gets."""
import mcp
from common import show
s = mcp.Server('p22')
s.call('new_project', {'name': 'w'}); s.call('open_project', {'name': 'w'}); s.call('new_document', {})
for i, n in enumerate(['+5V', '-12V', '3V3', 'V+', 'VDD_IO', '5V0']):
    s.call('add_component', {'type': 'R', 'name': f'R{i}', 'x': 100 + 200 * i, 'y': 100, 'properties': {'R': '1k'}})
    s.call('add_component', {'type': 'R', 'name': f'Q{i}', 'x': 200 + 200 * i, 'y': 160, 'properties': {'R': '1k'}})
    s.call('connect', {'from': f'R{i}.1', 'to': f'Q{i}.1'})
    r = s.call('set_label', {'at': f'R{i}.1', 'name': n})
    show(f'set_label {n!r} err={int(s.last_error)}', r, 300)
print(s.call('get_netlist', {})[:900])
s.close()
