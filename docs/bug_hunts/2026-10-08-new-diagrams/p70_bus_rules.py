"""Check Schematic's bus rules: three resistors whose wires end on a bus D[7:0] - one labelled D3 (a member), one D9 (not), one unlabelled."""
import json, mcp
from common import show
s = mcp.Server('p70')
s.call('new_project', {'name': 'b'}); s.call('open_project', {'name': 'b'}); s.call('new_document', {})
show('bus', s.call('add_painting', {'type': 'bus', 'name': 'D[7:0]', 'from': [100, 100], 'to': [700, 100]}), 200)
for i, (x, lab) in enumerate(((200, 'D3'), (400, 'D9'), (600, None))):
    s.call('add_component', {'type': 'R', 'name': f'R{i+1}', 'x': x, 'y': 240, 'properties': {'R': '1k'}})
    pin = next(q for c in s.call('get_schematic', {})['components'] if c['name'] == f'R{i+1}' for q in c['pins'] if q['pin'] == 1)
    w = s.call('add_wire', {'points': [[pin['x'], pin['y']], [pin['x'], 100]]})
    if s.last_error: print('wire', str(w)[:150])
    if lab:
        l = s.call('set_label', {'at': [pin['x'], 150], 'name': lab})
        if s.last_error: print('label', str(l)[:150])
    s.call('connect', {'from': f'R{i+1}.2', 'to': 'ground'})
c = s.call('check_schematic', {})
show('check_schematic', [(w.get('message')) for w in c.get('errors', []) + c.get('warnings', []) + c.get('notes', [])], 1500)
s.close()
