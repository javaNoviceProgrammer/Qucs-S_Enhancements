"""LTspice flags named as Qucs-S labels cannot be (+5V, -5V, 5V, V+, V-, 3V3, a.b, a b): each on its own resistor to ground - distinct nets after the import?"""
import mcp
from common import show
NAMES = ['+5V', '-5V', '5V', 'V+', 'V-', '3V3', 'a.b', 'a_b', 'VCC']
lines = ['Version 4', 'SHEET 1 2000 680']
for i, n in enumerate(NAMES):
    x = 100 + 150 * i
    lines += [f'FLAG {x + 16} 16 {n}', f'FLAG {x + 16} 96 0', f'SYMBOL res {x} 0 R0', f'SYMATTR InstName R{i + 1}', f'SYMATTR Value {i + 1}k']
lines += ['TEXT 0 300 Left 2 !.op']
s = mcp.Server('p69')
open(s.ws + '/fl.asc', 'w').write('\n'.join(lines) + '\n')
r = s.call('import_netlist', {'file': s.ws + '/fl.asc', 'save_as': 'fl.sch'})
show('notes', r.get('LTspice') if isinstance(r, dict) else r, 900)
n = s.call('get_netlist', {})
rl = [l.split()[:3] for l in str(n).splitlines() if l[:1] in 'Rr' and len(l.split()) > 2]
print('resistors and their nets:', rl)
nets = [x[1] if x[2] == '0' else x[2] for x in rl]
print('distinct nets:', len(set(nets)), 'of', len(NAMES))
s.close()
