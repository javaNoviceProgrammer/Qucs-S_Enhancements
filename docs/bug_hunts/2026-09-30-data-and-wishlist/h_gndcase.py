# What node a label becomes: Foo, GND without a ground symbol, GND with one connected (at a wire's end), and with one not connected.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
sim = sys.argv[1]
ws = HERE + '/ws_gc' + sim; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=%s\nRequireGround=false\nXyceExecutable=/usr/bin/true\nSpiceOpusExecutable=/usr/bin/true\n' % sim)
s = Server(ws)
HEAD = '<Qucs Schematic 26.1.4>\n<Components>\n'
V = '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n'
R = '  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
DC = '  <.DC DC1 1 200 40 0 40 0 0 "26.85" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "no" 0 "150" 0 "no" 0 "none" 0 "CroutLU" 0>\n'
cases = {'Foo': ('', 'Foo'), 'GND none': ('', 'GND'), 'GND at end': ('  <GND * 1 100 90 0 0 0 0>\n', 'GND'),
         'GND mid-wire': ('  <GND * 1 50 90 0 0 0 0>\n', 'GND'), 'none at end': ('  <GND * 1 100 90 0 0 0 0>\n', '')}
for k, (gnd, label) in cases.items():
    f = ws + '/c%d.sch' % len(os.listdir(ws))
    open(f, 'w').write(HEAD + V + R + DC + gnd + '</Components>\n<Wires>\n  <0 30 100 30 "out" 50 10 0 "">\n'
                       '  <0 90 100 90 "%s" 50 110 0 "">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n' % label)
    s.call('open_document', {'path': f}, ok=False)
    net = s.call('get_netlist', {'path': f}, ok=False)
    net = net if isinstance(net, str) else json.dumps(net)
    print(sim, k, '|', [l for l in net.splitlines() if l[:2] in ('V1', 'R1')])
s.close()
