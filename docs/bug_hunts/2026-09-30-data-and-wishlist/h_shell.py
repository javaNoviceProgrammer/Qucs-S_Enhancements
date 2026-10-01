# ngspice's shell command in a schematic's custom simulation: run by simulate, and said by nothing?
import os, sys, json, shutil, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
ws = HERE + '/ws_shell'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
marker = ws + '/shell-ran'
txt = ('<Qucs Schematic 26.1.4>\n<Components>\n'
       '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
       '  <GND * 1 0 90 0 0 0 0>\n'
       '  <.CUSTOMSIM OP1 1 130 870 0 51 0 0 "\\nop\\nshell touch %s\\nprint output v(out) > custom#op1#.print\\ndestroy all\\n" 1 "" 0 "custom#op1#.print" 0>\n' % marker +
       '</Components>\n<Wires>\n  <0 30 100 30 "out" 50 10 0 "">\n  <0 90 100 90 "" 0 0 0 "">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
open(ws + '/sh.sch', 'w').write(txt)
s = Server(ws)
s.call('open_document', {'path': ws + '/sh.sch'}, ok=False)
print('check:', [i['message'][:100] for k in ('errors', 'warnings', 'notes') for i in s.call('check_schematic', {}, ok=False).get(k, [])])
net = str(s.call('get_netlist', {}, ok=False)); print('netlist has shell:', 'shell touch' in net)
r = s.call('simulate', {}, ok=False)
print('simulate:', r.get('succeeded') if isinstance(r, dict) else r[:200], re.findall(r'[^."]*shell[^."]*', json.dumps(r))[:3])
print('marker made:', os.path.exists(marker))
s.close()
