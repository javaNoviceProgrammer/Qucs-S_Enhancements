# A ground symbol on a wire that carries a label (a file can have it; the GUI refuses to place one):
# the node the ground's net gets, for each simulator, and what Check Schematic says.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
sim = sys.argv[1]
ws = HERE + '/ws_gl_' + sim; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=%s\nXyceExecutable=/usr/bin/true\nSpiceOpusExecutable=/usr/bin/true\n' % sim)
s = Server(ws)
HEAD = '<Qucs Schematic 26.1.4>\n<Components>\n'
V = '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n'
R = '  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
DC = '  <.DC DC1 1 200 40 0 40 0 0 "26.85" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "no" 0 "150" 0 "no" 0 "none" 0 "CroutLU" 0>\n'
for label in ('GND', 'vss', 'Gnd'):
    f = ws + '/g_%s.sch' % label
    open(f, 'w').write(HEAD + V + R + DC + '  <GND * 1 50 90 0 0 0 0>\n</Components>\n<Wires>\n  <0 30 100 30 "out" 50 10 0 "">\n'
                       '  <0 90 100 90 "%s" 50 110 0 "">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n' % label)
    s.call('open_document', {'path': f}, ok=False)
    net = s.call('get_netlist', {'path': f}, ok=False)
    net = net if isinstance(net, str) else json.dumps(net)
    chk = s.call('check_schematic', {'path': f}, ok=False)
    print(sim, repr(label), '|', [l for l in net.splitlines() if l[:2] in ('V1', 'R1')],
          '| check:', [i['message'][:80] for k in ('errors', 'warnings') for i in (chk.get(k, []) if isinstance(chk, dict) else [])])
s.close()
