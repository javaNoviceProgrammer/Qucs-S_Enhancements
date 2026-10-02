import sys, json; sys.path.insert(0, sys.argv[1])
from probe import Probe, sch
p = Probe(sys.argv[1] + '/p13')
comps = ''.join('  <R R%d 1 %d 100 15 -26 0 1 "1k" 1>\n  <GND * 1 %d 160 0 0 0 0>\n' % (k, 100*k, 100*k) for k in range(1, 4))
wires = '  <100 70 200 70 "" 0 0 0 "">\n'
p.open('pv.sch', sch(comps, wires))
p.call('edit_component', {'name': 'R1', 'properties': {'R': '2k'}})          # real edit A
e, o = p.call('edit_component', {'name': 'R2', 'properties': {'R': '9k'}, 'preview': True})
print('preview:', e, o[:160].replace('\n', ' '))
e, o = p.call('undo', {}); print('undo after preview:', e, o[:200].replace('\n', ' '))
e, o = p.call('get_schematic', {'format': 'text'}); print([l.strip()[:40] for l in o.splitlines() if l.startswith('  <R ')])
# selection kept across a preview that deletes a selected part
p.call('select', {'names': ['R2', 'GND#2']})
e, o = p.call('delete', {'names': ['R2'], 'preview': True}); print('preview delete:', e, o[:120].replace('\n', ' '))
e, o = p.call('get_state', {}); sel = json.loads(o).get('selection') or json.loads(o).get('documents', [{}])[0].get('selected')
print('selection after preview:', json.dumps(sel)[:200])
e, o = p.call('move', {'selection': True, 'dx': 0, 'dy': 20, 'preview': True}); print('move sel preview:', e, o[:200].replace(chr(10), ' '))
for refs in (['GND#0'], ['GND#999'], ['gnd#1'], ['R1#2'], ['GND#'], ['#1'], ['GND#-1'], ['GND#1', 'GND#1', 'GND']):
    e, o = p.call('select', {'names': refs}); print('select', refs, '->', o[:170].replace('\n', ' '))
p.close()
