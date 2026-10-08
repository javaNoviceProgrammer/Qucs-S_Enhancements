"""View > Colour Wires by Net: which net names it takes for a supply (get_schematic's wire kinds)."""
import json, sys, mcp
from common import show
s = mcp.Server('p21')
s.call('new_project', {'name': 'w'}); s.call('open_project', {'name': 'w'}); s.call('new_document', {})
NAMES = ['VCC', 'VDD', '+5V', '-12V', '3V3', '+3V3', 'VEE', 'VSS', 'AVDD', 'VDD_IO', 'VBAT', 'VIN', 'VOUT', 'VREF', 'VBIAS', 'VSENSE', 'V1', 'V+', 'VGATE',
         'VCTRL', 'Vdiv', '5V_OUT', 'CLK', 'PWR_GOOD', 'VCC_EN', 'GND', 'AGND', 'GNDA', 'VNEG', 'VPP', 'VCO', 'VCO_OUT', 'VGA_IN', 'VLOAD']
for i, n in enumerate(NAMES):
    x = 100 + (i % 6) * 200; y = 100 + (i // 6) * 150
    s.call('add_component', {'type': 'R', 'name': f'R{i}', 'x': x, 'y': y, 'properties': {'R': '1k'}})
    s.call('add_component', {'type': 'R', 'name': f'Q{i}', 'x': x + 100, 'y': y + 60, 'properties': {'R': '1k'}})
    s.call('connect', {'from': f'R{i}.1', 'to': f'Q{i}.1'})
    s.call('set_label', {'at': f'R{i}.1', 'name': n})
    s.call('connect', {'from': f'R{i}.2', 'to': 'ground'}); s.call('connect', {'from': f'Q{i}.2', 'to': 'ground'})
g = s.call('get_schematic', {})
wires = g.get('wires', [])
print('wire keys', list(wires[0].keys()) if wires else None)
pins = {}
for comp in g['components']:
    if comp['name'].startswith('R') and comp['name'][1:].isdigit():
        p1 = next(q for q in comp['pins'] if q['pin'] == 1); pins[(p1['x'], p1['y'])] = NAMES[int(comp['name'][1:])]
kinds = {}
for w in wires:
    for end in ((w['x1'], w['y1']), (w['x2'], w['y2'])):
        if end in pins: kinds.setdefault(pins[end], set()).add(w.get('kind'))
show('kinds', {k: sorted(v, key=str) for k, v in kinds.items()}, 3000)
s.close()
