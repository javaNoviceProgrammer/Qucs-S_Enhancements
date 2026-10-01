# get_dataset of a kept run (keep_as): the trace names it gives - do they name the kept run, or the current one?
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
ws = HERE + '/ws_kt'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
shutil.copy('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/templates_ngspice/AC_Passive_analysis.sch', ws + '/a.sch')
s = Server(ws)
s.call('open_document', {'path': ws + '/a.sch'})
s.call('simulate', {'path': 'a.sch', 'keep_as': 'run1'}, ok=False)
d = s.call('get_dataset', {'path': 'run1.dat.ngspice'}, ok=False)
traces = [v.get('trace') for v in d.get('variables', [])][:3] if isinstance(d, dict) else d[:200]
print('get_dataset run1.dat.ngspice traces:', traces)
if isinstance(traces, list) and traces and traces[0]:
    r = s.call('add_trace', {'path': 'a.sch', 'diagram': 1, 'variable': traces[0]}, ok=False)
    print('that trace on the diagram reads:', json.dumps(r)[:200] if isinstance(r, dict) else r[:200])
s.close()
