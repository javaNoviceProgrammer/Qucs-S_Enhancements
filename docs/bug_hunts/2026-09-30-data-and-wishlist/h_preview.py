# A preview of a diagram on a data display that is open with unsaved changes (round 10 closed the ones it opened).
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_prev'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
shutil.copy('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/templates_ngspice/AC_Passive_analysis.sch', ws + '/a.sch')
s.call('open_document', {'path': ws + '/a.sch'})
r = s.call('add_diagram', {'path': 'a.sch', 'document': 'data_display', 'traces': ['v(out)']}, ok=False)
print('first diagram:', (r if isinstance(r, str) else r.get('note', ''))[:160])
st = s.call('get_state', {})
print('state:', json.dumps(st.get('documents', st))[:400])
dpl = [d for d in os.listdir(ws) if d.endswith('.dpl')]
print('dpl on disk:', dpl, [os.path.getsize(ws + '/' + d) for d in dpl])
r = s.call('add_diagram', {'path': 'a.sch', 'document': 'data_display', 'traces': ['v(in)'], 'preview': True}, ok=False)
print('preview:', json.dumps(r.get('would change') if isinstance(r, dict) else r)[:300])
sch = s.call('get_schematic', {'path': dpl[0] if dpl else 'AC_Passive_analysis.dpl'}, ok=False)
print('after the preview, diagrams on it:', len(sch.get('diagrams', [])) if isinstance(sch, dict) else sch[:200])
st = s.call('get_state', {})
print('state:', json.dumps(st.get('documents', st))[:400])
s.close()
