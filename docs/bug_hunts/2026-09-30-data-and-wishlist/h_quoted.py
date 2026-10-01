# A CSV whose header quotes a name with a comma in it (as the exporter, and Excel, write "S[1,1]" or "v(a,b)").
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_quoted'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
s.call('new_document', {}); s.call('save_document', {'as': 'p.sch'})
cases = {'q1.csv': 'f,"S[1,1]",y\n1,2,3\n2,3,4\n3,4,5\n',
         'q2.csv': '"v(a,b)",y\n1,3\n2,4\n3,5\n',
         'q3.csv': 'f,"real(S[2,1])","imag(S[2,1])"\n1,2,3\n2,3,4\n3,4,5\n'}
for f, text in cases.items():
    open(ws + '/' + f, 'w').write(text)
    r = s.call('import_data', {'file': f}, ok=False)
    print(f, '->', (r if isinstance(r, str) else json.dumps({k: r.get(k) for k in ('columns', 'traces', 'x', 'notes')})))
s.close()
