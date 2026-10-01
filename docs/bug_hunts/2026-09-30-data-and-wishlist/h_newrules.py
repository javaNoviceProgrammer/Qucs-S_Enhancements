# The wishlist's two new rules of Check Schematic over every example: silent AC sources, equations named as nets.
import os, sys, json, shutil, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_rules'; shutil.rmtree(ws, ignore_errors=True)
shutil.copytree('/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples', ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=%s\n' % (sys.argv[1] if len(sys.argv) > 1 else '1'))
s = Server(ws)
n = 0; hits = []
for f in sorted(glob.glob(ws + '/**/*.sch', recursive=True)):
    s.call('open_document', {'path': f}, ok=False)
    r = s.call('check_schematic', {'path': f}, ok=False)
    s.call('close_document', {'path': f, 'unsaved': 'discard'}, ok=False)
    if not isinstance(r, dict): continue
    n += 1
    for k in ('errors', 'warnings', 'notes'):
        for i in r.get(k, []):
            m = i['message']
            if 'AC magnitude' in m or 'AC source' in m or 'named as the net' in m or 'writes over' in m:
                hits.append((os.path.relpath(f, ws), k, m[:170]))
print(n, 'checked;', len(hits), 'from the two rules')
for h in hits: print(' ', h)
s.close()
