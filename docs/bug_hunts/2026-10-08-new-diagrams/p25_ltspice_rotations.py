"""LTspice rotations: a res, a cap and a voltage source at R0/R90/R180/R270, flags where LTspice puts their pins - does each join its flags?"""
import os, mcp
from common import show
PINS = {'res': [(16, 16), (16, 96)], 'cap': [(16, 0), (16, 64)], 'voltage': [(0, 16), (0, 96)]}
ROT = {'R0': lambda x, y: (x, y), 'R90': lambda x, y: (-y, x), 'R180': lambda x, y: (-x, -y), 'R270': lambda x, y: (y, -x)}
s = mcp.Server('p25')
for sym, pins in PINS.items():
    for rot, f in ROT.items():
        ox, oy = 400, 400
        (ax, ay), (bx, by) = [(ox + f(x, y)[0], oy + f(x, y)[1]) for x, y in pins]
        asc = (f'Version 4\nSHEET 1 880 680\nFLAG {ax} {ay} a\nFLAG {bx} {by} b\nSYMBOL {sym} {ox} {oy} {rot}\nSYMATTR InstName X1\nSYMATTR Value 1k\n')
        path = f'{s.ws}/{sym}_{rot}.asc'; open(path, 'w').write(asc)
        r = s.call('import_netlist', {'file': path, 'save_as': f'{sym}_{rot}.sch'})
        net = s.call('get_netlist', {}) if not s.last_error else ''
        line = ' | '.join(l.strip() for l in net.splitlines() if set(l.split()[1:3]) & {'a', 'b'}) if isinstance(net, str) else ''
        notes = [n for n in (r.get('LTspice', []) if isinstance(r, dict) else []) if 'joins nothing' in n or 'pin' in n]
        print(f'{sym:8s} {rot:5s} {line!s:40s} {notes[:2] if notes else ""}', flush=True)
        s.call('close_document', {})
s.close()
