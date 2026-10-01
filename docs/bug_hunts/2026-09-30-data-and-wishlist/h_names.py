# Copies and saves under names a trace cannot name (a ':' or '/' splits name:variable and simulator/...).
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_names'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
shutil.copy('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/templates_ngspice/AC_Passive_analysis.sch', ws + '/a.sch')
s.call('open_document', {'path': ws + '/a.sch'})
print('sim:', s.call('simulate', {'path': 'a.sch'}, ok=False).get('succeeded'))
for to in ('x:y', 'v(out)', 'p q', 'ngspice'):
    r = s.call('copy_document', {'path': 'a.sch', 'to': to}, ok=False)
    print(repr(to), '->', (r if isinstance(r, str) else json.dumps(r))[:200])
print(sorted(os.listdir(ws)))
# A trace of the copy's dataset, by its name.
for name in ('x:y', 'v(out)', 'p q'):
    r = s.call('add_diagram', {'path': 'a.sch', 'traces': ['%s:ac.v(out)' % name]}, ok=False)
    print('trace of', repr(name), '->', [(t.get('variable'), t.get('points'), t.get('no data')) for t in r['traces']] if isinstance(r, dict) else r[:200])
s.close()
