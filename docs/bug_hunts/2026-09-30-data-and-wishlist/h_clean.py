# clean_scratch with datasets: which files it trashes for a schematic whose Data Set is not its own name.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_clean'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
sch = open('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/templates_ngspice/AC_Passive_analysis.sch').read() if False else None
txt = ('<Qucs Schematic 26.1.4>\n<Properties>\n  <DataSet=run.dat>\n  <DataDisplay=amp.dpl>\n</Properties>\n<Components>\n'
       '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
       '  <GND * 1 0 90 0 0 0 0>\n  <.DC DC1 1 200 40 0 40 0 0 "26.85" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "no" 0 "150" 0 "no" 0 "none" 0 "CroutLU" 0>\n'
       '</Components>\n<Wires>\n  <0 30 100 30 "out" 50 10 0 "">\n  <0 90 100 90 "" 0 0 0 "">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
open(ws + '/amp.sch', 'w').write(txt)
open(ws + '/amp.dat', 'w').write('<Qucs Dataset 26.1.4>\n<indep x 1>\n1\n</indep>\n')   # (an old one of this name, say an import's)
s.call('open_document', {'path': ws + '/amp.sch'})
r = s.call('simulate', {'path': 'amp.sch'})
print('simulated:', r.get('succeeded'), r.get('dataset'))
print('before:', sorted(os.listdir(ws)))
print(s.call('clean_scratch', {'path': 'amp.sch', 'datasets': True}))
print('after:', sorted(os.listdir(ws)))
s.close()
