# add_diagram document: data_display for a schematic whose Data Set is not its own name.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_dpl'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
TR = '  <.TR TR1 1 200 40 0 71 0 0 "lin" 1 "0" 1 "1 ms" 1 "11" 0 "Trapezoidal" 0 "2" 0 "1 ns" 0 "1e-16" 0 "150" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "26.85" 0 "1e-3" 0 "1e-6" 0 "1" 0 "CroutLU" 0 "no" 0 "yes" 0 "0" 0>\n'
txt = ('<Qucs Schematic 26.1.4>\n<Properties>\n  <DataSet=run.dat>\n  <DataDisplay=amp.dpl>\n</Properties>\n<Components>\n'
       '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
       '  <GND * 1 0 90 0 0 0 0>\n' + TR + '</Components>\n<Wires>\n  <0 30 100 30 "out" 50 10 0 "">\n  <0 90 100 90 "" 0 0 0 "">\n</Wires>\n'
       '<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
open(ws + '/amp.sch', 'w').write(txt)
s.call('open_document', {'path': ws + '/amp.sch'})
print('sim:', s.call('simulate', {'path': 'amp.sch'}).get('dataset'))
r = s.call('add_diagram', {'path': 'amp.sch', 'traces': ['out']})
print('on the schematic:', [(t['variable'], t.get('points'), t.get('no data')) for t in r['traces']])
r = s.call('add_diagram', {'path': 'amp.sch', 'document': 'data_display', 'traces': ['out']})
print('on its data display:', [(t['variable'], t.get('points'), t.get('no data')) for t in r['traces']] if isinstance(r, dict) else r[:600])
print(open(ws + '/amp.dpl').read()[:300] if os.path.exists(ws + '/amp.dpl') else 'no dpl file')
s.call('save_document', {'path': 'amp.dpl'})
print(open(ws + '/amp.dpl').read()[:300])
s.close()
