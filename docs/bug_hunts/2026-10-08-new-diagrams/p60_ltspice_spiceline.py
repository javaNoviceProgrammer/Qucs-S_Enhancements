"""LTspice parts' extra attributes: a MOSFET's SpiceLine (W, L), a cap's Rser and a SpiceLine2, a voltage source's AC in its Value2 - kept, said, or dropped?"""
import mcp
from common import show
ASC = """Version 4
SHEET 1 880 680
WIRE 100 100 200 100
FLAG 100 180 0
FLAG 316 114 0
FLAG 316 50 g
WIRE 200 100 316 50
SYMBOL voltage 100 84 R0
SYMATTR InstName V1
SYMATTR Value 2
SYMATTR Value2 AC 1
SYMBOL cap 300 50 R0
SYMATTR InstName C1
SYMATTR Value 1u
SYMATTR SpiceLine V=50 Irms=1 Rser=0.01
SYMATTR SpiceLine2 Lser=1n
TEXT 0 300 Left 2 !.model MYN NMOS (VTO=1 KP=1e-4)
TEXT 0 330 Left 2 !.op
"""
s = mcp.Server('p60')
open(s.ws + '/sl.asc', 'w').write(ASC)
r = s.call('import_netlist', {'file': s.ws + '/sl.asc', 'save_as': 'sl.sch'})
show('import', r.get('LTspice') if isinstance(r, dict) else r, 1500)
n = s.call('get_netlist', {})
print('\n'.join(l for l in str(n).splitlines() if l[:1] in 'VMCvmc' or l.startswith('.model')))
sim = s.call('simulate', {'simulator': 'ngspice'})
print('run:', (sim.get('succeeded'), sim.get('errors'), (sim.get('last lines') or '')[-500:] if isinstance(sim.get('last lines'), str) else (sim.get('last lines') or [])[-8:]) if isinstance(sim, dict) else sim[:600])
print('notes:', r.get('LTspice') if isinstance(r, dict) else r)
s.close()
