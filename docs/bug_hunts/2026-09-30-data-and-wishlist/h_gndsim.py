# The named-ground fix (d62605f) under each SPICE simulator's netlist, and Check Schematic's view of the same nets.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mcp
from mcp import Server, HERE
sim = sys.argv[1]
ws = HERE + '/ws_gnd_' + sim; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
os.makedirs(HERE + '/settings/qucs', exist_ok=True)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=%s\nRequireGround=false\nXyceExecutable=/usr/bin/true\nSpiceOpusExecutable=/usr/bin/true\n' % sim)
s = Server(ws)
HEAD = '<Qucs Schematic 26.1.4>\n<Components>\n'
V = '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n'
R = '  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
TR = '  <.TR TR1 1 200 40 0 71 0 0 "lin" 1 "0" 1 "1 ms" 1 "11" 0 "Trapezoidal" 0 "2" 0 "1 ns" 0 "1e-16" 0 "150" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "26.85" 0 "1e-3" 0 "1e-6" 0 "1" 0 "CroutLU" 0 "no" 0 "yes" 0 "0" 0>\n'
for label in ('0', 'gnd', 'GND', 'Gnd'):
    f = ws + '/n_%s.sch' % label
    open(f, 'w').write(HEAD + V + R + TR + '</Components>\n<Wires>\n  <0 30 100 30 "out" 50 10 0 "">\n  <0 90 100 90 "%s" 50 110 0 "">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n' % label)
    s.call('open_document', {'path': f}, ok=False)
    net = s.call('get_netlist', {'path': f}, ok=False)
    net = net if isinstance(net, str) else json.dumps(net)
    lines = [l for l in net.splitlines() if l[:2] in ('V1', 'R1') or 'print' in l.lower() or 'PRINT' in l]
    chk = s.call('check_schematic', {'path': f}, ok=False)
    msgs = [i['message'][:90] for k in ('errors', 'warnings') for i in (chk.get(k, []) if isinstance(chk, dict) else [])]
    print(sim, repr(label), '|', lines, '| check:', msgs)
s.close()
