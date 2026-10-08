"""LTspice-only directives in an .asc: .step param, .meas, .four, .backanno, .wave - converted, said, or passed on to ngspice? Then the run."""
import mcp
from common import show
ASC = """Version 4
SHEET 1 880 680
WIRE 48 80 112 80
WIRE 192 80 240 80
FLAG 48 160 0
FLAG 240 144 0
FLAG 240 80 out
SYMBOL voltage 48 64 R0
SYMATTR InstName V1
SYMATTR Value 1
SYMBOL res 208 64 R90
SYMATTR InstName R1
SYMATTR Value {Rv}
SYMBOL cap 224 80 R0
SYMATTR InstName C1
SYMATTR Value 1u
TEXT 48 200 Left 2 !.tran 5m
TEXT 48 230 Left 2 !.param Rv=1k
TEXT 48 260 Left 2 !.step param Rv 1k 3k 1k
TEXT 48 290 Left 2 !.meas tran vmax MAX V(out)
TEXT 48 320 Left 2 !.four 1k V(out)
TEXT 48 350 Left 2 !.backanno
TEXT 48 380 Left 2 !.wave out.wav 16 44.1K V(out)
"""
s = mcp.Server('p59')
open(s.ws + '/st.asc', 'w').write(ASC)
r = s.call('import_netlist', {'file': s.ws + '/st.asc', 'save_as': 'st.sch'})
show('import', r.get('LTspice') if isinstance(r, dict) else r, 1500)
n = s.call('get_netlist', {})
print('\n'.join(l for l in str(n).splitlines() if l.startswith('.') or 'step' in l.lower() or 'meas' in l.lower()))
sim = s.call('simulate', {'simulator': 'ngspice'})
print('run:', sim.get('succeeded'), sim.get('errors'), (sim.get('warnings') or [])[:3])
s.close()
