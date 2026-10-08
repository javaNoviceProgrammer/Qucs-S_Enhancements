"""A subcircuit symbol's .asy with odd pins (SpiceOrder 0, -1, 1000, twice, none; huge coordinates; no PIN), imported with the .asc that places it - ASan build."""
import os, sys, mcp
from common import show
here = os.path.dirname(os.path.abspath(__file__))
ASC = 'Version 4\nSHEET 1 880 680\nFLAG 400 64 a\nFLAG 400 96 b\nFLAG 432 80 c\nSYMBOL mysub 400 64 R0\nSYMATTR InstName U1\nTEXT 0 200 Left 2 !.op\n'
def asy(pins, extra='SYMATTR Prefix X\nSYMATTR SpiceModel MYSUB\n'):
    t = 'Version 4\nSymbolType CELL\n'
    for (x, y, name, order) in pins:
        t += f'PIN {x} {y} NONE 8\n'
        if name is not None: t += f'PINATTR PinName {name}\n'
        if order is not None: t += f'PINATTR SpiceOrder {order}\n'
    return t + extra
CASES = {
  'order 0': [(0, 0, 'A', 0), (0, 32, 'B', 1), (32, 16, 'C', 2)],
  'order -1': [(0, 0, 'A', -1), (0, 32, 'B', 1), (32, 16, 'C', 2)],
  'order 1000': [(0, 0, 'A', 1000), (0, 32, 'B', 1), (32, 16, 'C', 2)],
  'order 2147483647': [(0, 0, 'A', 2147483647), (0, 32, 'B', 1), (32, 16, 'C', 2)],
  'order twice': [(0, 0, 'A', 1), (0, 32, 'B', 1), (32, 16, 'C', 2)],
  'no order': [(0, 0, 'A', None), (0, 32, 'B', None), (32, 16, 'C', None)],
  'order text': [(0, 0, 'A', 'x'), (0, 32, 'B', 1), (32, 16, 'C', 2)],
  'no name': [(0, 0, None, 1), (0, 32, None, 2), (32, 16, None, 3)],
  'huge coords': [(2147483647, -2147483648, 'A', 1), (0, 32, 'B', 2), (32, 16, 'C', 3)],
  'two pins one place': [(0, 0, 'A', 1), (0, 0, 'B', 2), (32, 16, 'C', 3)],
  'no pins': [],
  '300 pins': [(i, 1000 + i, f'P{i}', i + 1) for i in range(300)],
}
for name, pins in CASES.items():
    s = mcp.Server('p31')
    open(s.ws + '/mysub.asy', 'w').write(asy(pins)); open(s.ws + '/t.asc', 'w').write(ASC)
    try:
        r = s.call('import_netlist', {'file': s.ws + '/t.asc', 'save_as': 't.sch', 'symbols': s.ws})
        net = s.call('get_netlist', {}) if not s.last_error else ''
        x = next((l.strip() for l in net.splitlines() if l.upper().startswith('XU1') or l.upper().startswith('X1') or ' MYSUB' in l.upper()), '-') if isinstance(net, str) else '-'
        lt = [n for n in r.get('LTspice', [])][1:3] if isinstance(r, dict) else str(r)[:150]
        err = open(s.root + '/server.err', errors='replace').read()
        san = [l for l in err.splitlines() if 'runtime error' in l or 'Sanitizer' in l][:2]
        print(f'{name:20s} {x[:60]:60s} {san if san else ""} {str(lt)[:160]}', flush=True)
    except Exception as e:
        print(f'{name:20s} SERVER DIED', [l for l in open(s.root + '/server.err', errors='replace').read().splitlines() if 'runtime error' in l or 'Sanitizer' in l or 'Fatal' in l][:3], flush=True)
    s.close()
