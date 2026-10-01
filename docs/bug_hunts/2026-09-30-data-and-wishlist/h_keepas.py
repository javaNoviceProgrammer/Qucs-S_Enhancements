# simulate keep_as a name an imported dataset has: is the kept run reachable as name:variable?
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_keep'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
shutil.copy('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/templates_ngspice/AC_Passive_analysis.sch', ws + '/a.sch')
open(ws + '/run1.csv', 'w').write('f,g\n1,2\n2,3\n')
s.call('open_document', {'path': ws + '/a.sch'})
print('import:', json.dumps(s.call('import_data', {'file': 'run1.csv'}, ok=False))[:100])
r = s.call('simulate', {'path': 'a.sch', 'keep_as': 'run1'}, ok=False)
print('simulate keep_as run1:', r.get('succeeded') if isinstance(r, dict) else r[:300], r.get('kept as') if isinstance(r, dict) else '')
print(sorted(os.listdir(ws)))
r = s.call('add_trace', {'path': 'a.sch', 'diagram': 1, 'variable': 'run1:ac.v(out)'}, ok=False)
print('trace run1:ac.v(out):', json.dumps(r)[:300])
s.close()
