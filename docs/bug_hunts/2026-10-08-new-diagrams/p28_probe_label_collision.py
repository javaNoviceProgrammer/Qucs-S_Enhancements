"""Probing an unnamed net labels it first: can the name it picks be one already used (two nets made one)? A ladder of R's, labels net1..net3 taken."""
import mcp
from common import show
s = mcp.Server('p28')
s.call('new_project', {'name': 'pl'}); s.call('open_project', {'name': 'pl'}); s.call('new_document', {})
s.call('add_component', {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
for i in range(1, 6):
    s.call('add_component', {'type': 'R', 'name': f'R{i}', 'x': 100 + 120 * i, 'y': 120, 'properties': {'R': '1k'}})
s.call('connect', {'from': 'V1.1', 'to': 'R1.1'}); s.call('connect', {'from': 'V1.2', 'to': 'ground'})
for i in range(1, 5): s.call('connect', {'from': f'R{i}.2', 'to': f'R{i+1}.1'})
s.call('connect', {'from': 'R5.2', 'to': 'ground'})
# labels on two nets with the names a probe might pick
for i, name in ((1, 'net1'), (2, 'n1'), (3, 'probe1')):
    show(f'label R{i}.2 {name}', s.call('set_label', {'at': f'R{i}.2', 'name': name}), 120)
s.call('add_analysis', {'kind': 'op'}); s.call('save_document', {'as': 'lad.sch'})
before = s.call('get_netlist', {})
show('netlist before', '\n'.join(l for l in before.splitlines() if l[:1] in 'RV'), 600)
pin = next(q for c in s.call('get_schematic', {})['components'] if c['name'] == 'R4' for q in c['pins'] if q['pin'] == 2)
g = s.call('get_schematic', {})
w = next(w for w in g['wires'] if (w['x1'], w['y1']) == (pin['x'], pin['y']) or (w['x2'], w['y2']) == (pin['x'], pin['y']))
mid = [(w['x1'] + w['x2']) // 2, (w['y1'] + w['y2']) // 2]
print('wire', w, 'mid', mid)
for what in (mid,):
    r = s.call('probe', {'what': what})
    show(f'probe {what}', r, 500)
after = s.call('get_netlist', {})
show('netlist after', '\n'.join(l for l in after.splitlines() if l[:1] in 'RV'), 600)
g = s.call('get_schematic', {})
pins = {c['name']: {q['pin']: q.get('net') for q in c['pins']} for c in g['components']}
show('get_schematic: each pin net', pins, 900)
show('get_schematic nets', g.get('nets'), 900)
s.close()
