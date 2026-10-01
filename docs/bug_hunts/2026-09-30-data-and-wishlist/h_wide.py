# import_data of a CSV of 5,000 columns: the size of the answer (traces and variables are cut at 100).
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
ws = HERE + '/ws_wide'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
n = 5000
open(ws + '/w.csv', 'w').write('t,' + ','.join('column_number_%d' % i for i in range(n)) + '\n' + ''.join(str(r) + ',' + ','.join(str(r * i) for i in range(n)) + '\n' for r in range(3)))
s = Server(ws)
s.call('new_document', {}); s.call('save_document', {'as': 'p.sch'})
m = s.rpc('tools/call', {'name': 'import_data', 'arguments': {'file': 'w.csv'}})
text = '\n'.join(c.get('text', '') for c in m['result']['content'])
o = json.loads(text) if text.startswith('{') else {}
print('answer chars:', len(text), '| columns listed:', len(o.get('columns', [])), '| variables:', len(o.get('variables', [])), '| traces:', len(o.get('traces', [])), '| trimmed:', 'trimmed' in text[:20000])
s.close()
