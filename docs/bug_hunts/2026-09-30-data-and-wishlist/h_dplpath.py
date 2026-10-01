# A Data Display setting outside the folder: what add_diagram document: data_display and new_document data_display make.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
ws = HERE + '/ws_dplp/in'; shutil.rmtree(HERE + '/ws_dplp', ignore_errors=True); os.makedirs(ws)
s = Server(ws)
for dd in ('../out.dpl', 'deep/x.dpl', 'amp.txt'):
    txt = ('<Qucs Schematic 26.1.4>\n<Properties>\n  <DataSet=amp.dat>\n  <DataDisplay=%s>\n</Properties>\n<Components>\n' % dd +
           '  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
    f = ws + '/amp%d.sch' % len(dd)
    open(f, 'w').write(txt)
    s.call('open_document', {'path': f}, ok=False)
    r = s.call('add_diagram', {'path': f, 'document': 'data_display'}, ok=False)
    print(repr(dd), '->', (r if isinstance(r, str) else (r.get('note') or json.dumps(r)))[:200])
print(sorted(os.path.relpath(os.path.join(dp, x), HERE + '/ws_dplp') for dp, dn, fn in os.walk(HERE + '/ws_dplp') for x in fn))
s.close()
