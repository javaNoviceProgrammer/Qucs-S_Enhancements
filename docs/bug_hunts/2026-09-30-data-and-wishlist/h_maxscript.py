# max_chars inside run_script's qucs.call (a direct call and batch take it; round 11).
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_ms'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
shutil.copy('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples/templates_ngspice/AC_Passive_analysis.sch', ws + '/a.sch')
s.call('open_document', {'path': ws + '/a.sch'})
for v in (300, '"300"'):
    code = 'const r = qucs.call("get_schematic", {max_chars: %s}); JSON.stringify(r).length + " " + JSON.stringify(r).slice(0, 160)' % v
    r = s.call('run_script', {'script': code}, ok=False)
    print(v, '->', (r if isinstance(r, str) else json.dumps(r))[:400])
r = s.call('get_schematic', {})
print('whole get_schematic length:', len(r) if isinstance(r, str) else len(json.dumps(r)))
s.close()
