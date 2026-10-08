"""LTspice parts that name a model of LTspice's own library (a 1N4148, a 2N3904, an LT1001): does the import say the model is missing, or the run?"""
import mcp
from common import show
ASC = """Version 4
SHEET 1 880 680
WIRE 100 100 200 100
FLAG 100 180 0
FLAG 216 160 0
FLAG 200 100 a
SYMBOL voltage 100 84 R0
SYMATTR InstName V1
SYMATTR Value 1
SYMBOL diode 200 96 R0
SYMATTR InstName D1
SYMATTR Value 1N4148
TEXT 0 300 Left 2 !.op
"""
s = mcp.Server('p62')
open(s.ws + '/md.asc', 'w').write(ASC)
r = s.call('import_netlist', {'file': s.ws + '/md.asc', 'save_as': 'md.sch'})
show('notes', r.get('LTspice') if isinstance(r, dict) else r, 900)
n = s.call('get_netlist', {}); print([l for l in str(n).splitlines() if l[:1] in 'DdVv' or l.startswith('.')][:6])
c = s.call('check_schematic', {}); print('check:', c.get('found'), [e.get('message') for e in c.get('errors', []) + c.get('warnings', [])][:3])
sim = s.call('simulate', {'simulator': 'ngspice'})
print('run:', sim.get('succeeded') if isinstance(sim, dict) else sim[:300], (sim.get('errors') if isinstance(sim, dict) else ''))
s.close()
