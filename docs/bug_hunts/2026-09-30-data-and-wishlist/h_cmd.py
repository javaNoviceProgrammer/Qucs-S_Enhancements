# A schematic's "System command" component (CMD): does Claude's simulate run it, and does anything say so?
import os, sys, json, shutil, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
ws = HERE + '/ws_cmd'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
marker = ws + '/cmd-ran'
TR = '  <.TR TR1 1 200 40 0 71 0 0 "lin" 1 "0" 1 "1 ms" 1 "11" 0 "Trapezoidal" 0 "2" 0 "1 ns" 0 "1e-16" 0 "150" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "26.85" 0 "1e-3" 0 "1e-6" 0 "1" 0 "CroutLU" 0 "no" 0 "yes" 0 "0" 0>\n'
txt = ('<Qucs Schematic 26.1.4>\n<Components>\n'
       '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
       '  <GND * 1 0 90 0 0 0 0>\n' + TR +
       '  <CMD CMD1 1 300 200 0 0 0 0 "touch %s" 1 "no" 1 "no" 0>\n' % marker +
       '</Components>\n<Wires>\n  <0 30 100 30 "out" 50 10 0 "">\n  <0 90 100 90 "" 0 0 0 "">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
open(ws + '/cmd.sch', 'w').write(txt)
s = Server(ws)
print('open:', str(s.call('open_document', {'path': ws + '/cmd.sch'}, ok=False))[:150])
print('check:', [i['message'][:100] for k in ('errors', 'warnings', 'notes') for i in s.call('check_schematic', {}, ok=False).get(k, [])])
print('netlist mentions CMD:', 'touch' in str(s.call('get_netlist', {}, ok=False)))
r = s.call('simulate', {}, ok=False)
time.sleep(1.5)
print('simulate:', r.get('succeeded') if isinstance(r, dict) else r[:200])
import re
print('  what it says of it:', re.findall(r'[^."]*(?:ommand|CMD)[^."]*', json.dumps(r))[:4])
print('marker made:', os.path.exists(marker))
s.close()
