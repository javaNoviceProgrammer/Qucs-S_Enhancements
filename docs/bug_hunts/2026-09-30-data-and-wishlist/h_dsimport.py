# import_data under the name a schematic's Data Set has (run.dat for p.sch): taken?
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=8\n')
ws = HERE + '/ws_dsi'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(ws + '/p.sch', 'w').write('<Qucs Schematic 26.1.4>\n<Properties>\n  <DataSet=run.dat>\n</Properties>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
open(ws + '/m.csv', 'w').write('t,a\n0,1\n1,2\n')
s = Server(ws)
s.call('open_document', {'path': ws + '/p.sch'})
r = s.call('import_data', {'file': 'm.csv', 'name': 'run'}, ok=False)
print('import as run, p.sch\'s Data Set:', (r if isinstance(r, str) else 'taken -> ' + r.get('file', '')))
s.close()
